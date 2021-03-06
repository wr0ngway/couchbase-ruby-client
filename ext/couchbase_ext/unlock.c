/* vim: ft=c et ts=8 sts=4 sw=4 cino=
 *
 *   Copyright 2011, 2012 Couchbase, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "couchbase_ext.h"

    void
unlock_callback(lcb_t handle, const void *cookie, lcb_error_t error, const lcb_unlock_resp_t *resp)
{
    struct context_st *ctx = (struct context_st *)cookie;
    struct bucket_st *bucket = ctx->bucket;
    VALUE key, *rv = ctx->rv, exc = Qnil, res;

    ctx->nqueries--;
    key = STR_NEW((const char*)resp->v.v0.key, resp->v.v0.nkey);
    strip_key_prefix(bucket, key);

    if (error != LCB_KEY_ENOENT || !ctx->quiet) {
        exc = cb_check_error(error, "failed to unlock value", key);
        if (exc != Qnil) {
            rb_ivar_set(exc, id_iv_operation, sym_unlock);
            if (NIL_P(ctx->exception)) {
                ctx->exception = cb_gc_protect(bucket, exc);
            }
        }
    }

    if (bucket->async) {    /* asynchronous */
        if (ctx->proc != Qnil) {
            res = rb_class_new_instance(0, NULL, cResult);
            rb_ivar_set(res, id_iv_error, exc);
            rb_ivar_set(res, id_iv_operation, sym_unlock);
            rb_ivar_set(res, id_iv_key, key);
            cb_proc_call(ctx->proc, 1, res);
        }
    } else {                /* synchronous */
        rb_hash_aset(*rv, key, (error == LCB_SUCCESS) ? Qtrue : Qfalse);
    }
    if (ctx->nqueries == 0) {
        cb_gc_unprotect(bucket, ctx->proc);
    }
    (void)handle;
}

/*
 * Unlock key
 *
 * @since 1.2.0
 *
 * The +unlock+ method allow you to unlock key once locked by {Bucket#get}
 * with +:lock+ option.
 *
 * @overload unlock(key, options = {})
 *   @param key [String, Symbol] Key used to reference the value.
 *   @param options [Hash] Options for operation.
 *   @option options [Fixnum] :cas The CAS value must match the current one
 *     from the storage.
 *   @option options [true, false] :quiet (self.quiet) If set to +true+, the
 *     operation won't raise error for missing key, it will return +nil+.
 *
 *   @return [true, false] +true+ if the operation was successful and +false+
 *     otherwise.
 *
 *   @raise [Couchbase::Error::Connect] if connection closed (see {Bucket#reconnect})
 *
 *   @raise [ArgumentError] when passing the block in synchronous mode
 *
 *   @raise [Couchbase::Error::NotFound] if key(s) not found in the storage
 *
 *   @raise [Couchbase::Error::TemporaryFail] if either the key wasn't
 *      locked or given CAS value doesn't match to actual in the storage
 *
 *   @example Unlock the single key
 *     val, _, cas = c.get("foo", :lock => true, :extended => true)
 *     c.unlock("foo", :cas => cas)
 *
 * @overload unlock(keys)
 *   @param keys [Hash] The Hash where keys represent the keys in the
 *     database, values -- the CAS for corresponding key.
 *
 *   @yieldparam ret [Result] the result of operation for each key in
 *     asynchronous mode (valid attributes: +error+, +operation+, +key+).
 *
 *   @return [Hash] Mapping keys to result of unlock operation (+true+ if the
 *     operation was successful and +false+ otherwise)
 *
 *   @example Unlock several keys
 *     c.unlock("foo" => cas1, :bar => cas2) #=> {"foo" => true, "bar" => true}
 *
 *   @example Unlock several values in async mode
 *     c.run do
 *       c.unlock("foo" => 10, :bar => 20) do |ret|
 *          ret.operation   #=> :unlock
 *          ret.success?    #=> true
 *          ret.key         #=> "foo" and "bar" in separate calls
 *       end
 *     end
 *
 */
   VALUE
cb_bucket_unlock(int argc, VALUE *argv, VALUE self)
{
    struct bucket_st *bucket = DATA_PTR(self);
    struct context_st *ctx;
    VALUE args, rv, proc, exc;
    lcb_error_t err;
    struct params_st params;

    if (bucket->handle == NULL) {
        rb_raise(eConnectError, "closed connection");
    }
    rb_scan_args(argc, argv, "0*&", &args, &proc);
    if (!bucket->async && proc != Qnil) {
        rb_raise(rb_eArgError, "synchronous mode doesn't support callbacks");
    }
    rb_funcall(args, id_flatten_bang, 0);
    memset(&params, 0, sizeof(struct params_st));
    params.type = cmd_unlock;
    params.bucket = bucket;
    cb_params_build(&params, RARRAY_LEN(args), args);
    ctx = xcalloc(1, sizeof(struct context_st));
    if (ctx == NULL) {
        rb_raise(eClientNoMemoryError, "failed to allocate memory for context");
    }
    ctx->proc = cb_gc_protect(bucket, proc);
    ctx->bucket = bucket;
    rv = rb_hash_new();
    ctx->rv = &rv;
    ctx->exception = Qnil;
    ctx->quiet = params.cmd.unlock.quiet;
    ctx->nqueries = params.cmd.unlock.num;
    err = lcb_unlock(bucket->handle, (const void *)ctx,
            params.cmd.unlock.num, params.cmd.unlock.ptr);
    cb_params_destroy(&params);
    exc = cb_check_error(err, "failed to schedule unlock request", Qnil);
    if (exc != Qnil) {
        xfree(ctx);
        rb_exc_raise(exc);
    }
    bucket->nbytes += params.npayload;
    if (bucket->async) {
        maybe_do_loop(bucket);
        return Qnil;
    } else {
        if (ctx->nqueries > 0) {
            /* we have some operations pending */
            lcb_wait(bucket->handle);
        }
        exc = ctx->exception;
        xfree(ctx);
        if (exc != Qnil) {
            rb_exc_raise(cb_gc_unprotect(bucket, exc));
        }
        if (bucket->exception != Qnil) {
            rb_exc_raise(bucket->exception);
        }
        if (params.cmd.unlock.num > 1) {
            return rv;  /* return as a hash {key => true, ...} */
        } else {
            VALUE vv = Qnil;
            rb_hash_foreach(rv, cb_first_value_i, (VALUE)&vv);
            return vv;
        }
    }
}


