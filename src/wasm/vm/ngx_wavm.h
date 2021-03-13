#ifndef _NGX_WAVM_H_INCLUDED_
#define _NGX_WAVM_H_INCLUDED_


#include <ngx_wasm.h>
#include <ngx_wavm_host.h>


#define NGX_WAVM_OK                  NGX_OK
#define NGX_WAVM_ERROR               NGX_ERROR
#define NGX_WAVM_AGAIN               NGX_AGAIN
#define NGX_WAVM_BUSY                NGX_BUSY
#define NGX_WAVM_SENT_LAST           NGX_DONE
#define NGX_WAVM_BAD_CTX             NGX_DECLINED
#define NGX_WAVM_BAD_USAGE           NGX_ABORT

#define ngx_wavm_state(m, s)         (m->state & s)

#define ngx_wavm_err_init(err)       ngx_memzero(err, sizeof(ngx_wavm_err_t))


typedef enum {
    NGX_WAVM_INIT = (1 << 0),
    NGX_WAVM_READY = (1 << 1),
} ngx_wavm_state;


typedef enum {
    NGX_WAVM_MODULE_ISWAT = (1 << 0),
    NGX_WAVM_MODULE_LOADED = (1 << 1),
    NGX_WAVM_MODULE_INVALID = (1 << 2),
    NGX_WAVM_MODULE_READY = (1 << 3),
} ngx_wavm_module_state;


typedef struct {
    ngx_wavm_t                        *vm;
    ngx_wavm_instance_t               *instance;
    ngx_log_t                         *orig_log;
} ngx_wavm_log_ctx_t;


struct ngx_wavm_funcref_s {
    ngx_wavm_module_t                 *module;
    ngx_str_node_t                     sn;         /* module->funcs */

    ngx_str_t                          name;
    ngx_uint_t                         exports_idx;
};


struct ngx_wavm_func_s {
    ngx_wavm_instance_t               *instance;

    const wasm_name_t                 *name;
    wasm_func_t                       *func;
    const wasm_valtype_vec_t          *argstypes;
    wasm_val_vec_t                     args;
    wasm_val_vec_t                     rets;
};


struct ngx_wavm_instance_s {
    ngx_wavm_ctx_t                    *ctx;
    ngx_wavm_module_t                 *module;
    ngx_queue_t                        q;          /* vm->instances */

    ngx_pool_t                        *pool;
    ngx_log_t                         *log;
    ngx_wavm_log_ctx_t                 log_ctx;
    ngx_wavm_hfunc_tctx_t             *tctxs;
    wasm_instance_t                   *instance;
    wasm_memory_t                     *memory;
    wasm_extern_vec_t                  env;
    wasm_extern_vec_t                  exports;
    ngx_wavm_func_t                  **funcs;

    ngx_str_t                          trapmsg;
    u_char                            *trapbuf;
    u_char                            *mem_offset;
};


struct ngx_wavm_ctx_s {
    ngx_pool_t                        *pool;
    ngx_log_t                         *log;
    void                              *data;

    ngx_wavm_t                        *vm;
    wasm_store_t                      *store;
    ngx_wavm_instance_t              **instances;
};


struct ngx_wavm_linked_module_s {
    ngx_wavm_module_t                 *module;
    ngx_queue_t                        q;          /* module->lmodules */

    ngx_uint_t                         idx;
    ngx_array_t                       *hfuncs_imports;
};


struct ngx_wavm_module_s {
    ngx_wavm_t                        *vm;
    ngx_str_node_t                     sn;         /* vm->modules_tree */

    ngx_uint_t                         state;
    ngx_str_t                          name;
    ngx_str_t                          path;
    wasm_byte_vec_t                    bytes;
    wasm_module_t                     *module;
    wasm_importtype_vec_t              imports;
    wasm_exporttype_vec_t              exports;
    ngx_wavm_funcref_t                *f_start;
    ngx_rbtree_t                       funcs_tree;
    ngx_rbtree_node_t                  funcs_sentinel;
    ngx_queue_t                        lmodules;
};


struct ngx_wavm_s {
    const ngx_str_t                   *name;
    ngx_uint_t                         state;
    ngx_pool_t                        *pool;
    ngx_log_t                         *log;
    ngx_wavm_log_ctx_t                 log_ctx;
    ngx_uint_t                         lmodules_max;
    ngx_rbtree_t                       modules_tree;
    ngx_rbtree_node_t                  modules_sentinel;
    ngx_queue_t                        instances;
    ngx_wavm_host_def_t               *core_host;
    wasm_engine_t                     *engine;
    wasm_store_t                      *store;
};


ngx_wavm_t *ngx_wavm_create(ngx_cycle_t *cycle, const ngx_str_t *name,
    ngx_wavm_host_def_t *core_host);
ngx_int_t ngx_wavm_init(ngx_wavm_t *vm);
ngx_int_t ngx_wavm_load(ngx_wavm_t *vm);
void ngx_wavm_destroy(ngx_wavm_t *vm);


ngx_int_t ngx_wavm_module_add(ngx_wavm_t *vm, ngx_str_t *name, ngx_str_t *path);
ngx_wavm_module_t *ngx_wavm_module_lookup(ngx_wavm_t *vm, ngx_str_t *name);
ngx_wavm_linked_module_t *ngx_wavm_module_link(ngx_wavm_module_t *module,
    ngx_wavm_host_def_t *host);
ngx_wavm_funcref_t *ngx_wavm_module_func_lookup(ngx_wavm_module_t *module,
    ngx_str_t *name);


ngx_int_t ngx_wavm_ctx_init(ngx_wavm_t *vm, ngx_wavm_ctx_t *ctx);
void ngx_wavm_ctx_destroy(ngx_wavm_ctx_t *ctx);
static ngx_inline void
ngx_wavm_ctx_update(ngx_wavm_ctx_t *ctx, ngx_log_t *log, void *data)
{
    size_t                i;
    ngx_wavm_instance_t  *instance;

    ctx->log = log;
    ctx->data = data;

    for (i = 0; i < ctx->vm->lmodules_max; i++) {
        instance = ctx->instances[i];
        instance->log_ctx.orig_log = log;
    }
}


ngx_wavm_instance_t *ngx_wavm_instance_create(ngx_wavm_linked_module_t *lmodule,
    ngx_wavm_ctx_t *ctx);
ngx_int_t ngx_wavm_instance_call_func(ngx_wavm_instance_t *instance,
    ngx_wavm_func_t *f, wasm_val_vec_t **rets, ...);
ngx_int_t ngx_wavm_instance_call_func_vec(ngx_wavm_instance_t *instance,
    ngx_wavm_func_t *f, wasm_val_vec_t **rets, wasm_val_vec_t *args);
ngx_int_t ngx_wavm_instance_call_funcref(ngx_wavm_instance_t *instance,
    ngx_wavm_funcref_t *funcref, wasm_val_vec_t **rets, ...);
ngx_int_t ngx_wavm_instance_call_funcref_vec(ngx_wavm_instance_t *instance,
    ngx_wavm_funcref_t *funcref, wasm_val_vec_t **rets, wasm_val_vec_t *args);
void ngx_wavm_instance_destroy(ngx_wavm_instance_t *instance);


#endif /* _NGX_WAVM_H_INCLUDED_ */
