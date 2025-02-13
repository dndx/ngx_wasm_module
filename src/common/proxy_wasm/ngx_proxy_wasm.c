#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include <ngx_proxy_wasm.h>
#include <ngx_proxy_wasm_properties.h>
#ifdef NGX_WASM_HTTP
#include <ngx_http_proxy_wasm.h>
#endif


static ngx_int_t ngx_proxy_wasm_init_abi(ngx_proxy_wasm_filter_t *filter);
static ngx_int_t ngx_proxy_wasm_start_filter(ngx_proxy_wasm_filter_t *filter);
static void ngx_proxy_wasm_instance_destroy(ngx_proxy_wasm_instance_t *ictx);
static ngx_proxy_wasm_err_e ngx_proxy_wasm_on_start(
    ngx_proxy_wasm_instance_t *ictx, ngx_proxy_wasm_filter_t *filter,
    unsigned start);
static void ngx_proxy_wasm_on_log(ngx_proxy_wasm_exec_t *pwexec);
static void ngx_proxy_wasm_on_done(ngx_proxy_wasm_exec_t *pwexec);
static ngx_int_t ngx_proxy_wasm_on_tick(ngx_proxy_wasm_exec_t *pwexec);


static ngx_rbtree_t       ngx_proxy_wasm_filters_rbtree;
static ngx_rbtree_node_t  ngx_proxy_wasm_filters_sentinel;


static ngx_proxy_wasm_filter_t *
ngx_proxy_wasm_filter_lookup(ngx_uint_t id)
{
    ngx_rbtree_t             *rbtree;
    ngx_rbtree_node_t        *node, *sentinel;
    ngx_proxy_wasm_filter_t  *filter;

    rbtree = &ngx_proxy_wasm_filters_rbtree;
    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (id != node->key) {
            node = (id < node->key) ? node->left : node->right;
            continue;
        }

        filter = ngx_rbtree_data(node, ngx_proxy_wasm_filter_t, node);

        return filter;
    }

    return NULL;
}


static ngx_proxy_wasm_exec_t *
ngx_proxy_wasm_root_lookup(ngx_proxy_wasm_instance_t *ictx, ngx_uint_t id)
{
    ngx_rbtree_t           *rbtree;
    ngx_rbtree_node_t      *node, *sentinel;
    ngx_proxy_wasm_exec_t  *pwexec;

    rbtree = &ictx->root_ctxs;
    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (id != node->key) {
            node = (id < node->key) ? node->left : node->right;
            continue;
        }

        pwexec = ngx_rbtree_data(node, ngx_proxy_wasm_exec_t, node);

        return pwexec;
    }

    return NULL;
}


static ngx_proxy_wasm_exec_t *
ngx_proxy_wasm_ctx_lookup(ngx_proxy_wasm_instance_t *ictx, ngx_uint_t id)
{
    ngx_rbtree_t           *rbtree;
    ngx_rbtree_node_t      *node, *sentinel;
    ngx_proxy_wasm_exec_t  *pwexec;

    rbtree = &ictx->tree_ctxs;
    node = rbtree->root;
    sentinel = rbtree->sentinel;

    while (node != sentinel) {

        if (id != node->key) {
            node = (id < node->key) ? node->left : node->right;
            continue;
        }

        pwexec = ngx_rbtree_data(node, ngx_proxy_wasm_exec_t, node);

        return pwexec;
    }

    return NULL;
}


void
ngx_proxy_wasm_init(ngx_conf_t *cf)
{
    ngx_rbtree_init(&ngx_proxy_wasm_filters_rbtree,
                    &ngx_proxy_wasm_filters_sentinel,
                    ngx_rbtree_insert_value);

    ngx_proxy_wasm_properties_init(cf);
}


void
ngx_proxy_wasm_exit()
{
    ngx_rbtree_node_t        **root_node, **sentinel, *node;
    ngx_proxy_wasm_filter_t   *filter;

    root_node = &ngx_proxy_wasm_filters_rbtree.root;
    sentinel = &ngx_proxy_wasm_filters_rbtree.sentinel;

    while (*root_node != *sentinel) {
        node = ngx_rbtree_min(*root_node, *sentinel);
        filter = ngx_rbtree_data(node, ngx_proxy_wasm_filter_t, node);

        ngx_rbtree_delete(&ngx_proxy_wasm_filters_rbtree, node);

        ngx_log_debug1(NGX_LOG_DEBUG_WASM, filter->log, 0,
                       "proxy_wasm releasing \"%V\" filter instance",
                       filter->name);

        ngx_proxy_wasm_store_destroy(filter->store);
    }
}


ngx_uint_t
ngx_proxy_wasm_id(ngx_str_t *name, ngx_str_t *config, uintptr_t data)
{
    u_char      buf[NGX_INT64_LEN];
    uint32_t    hash;
    ngx_str_t   str;

    str.data = buf;
    str.len = ngx_sprintf(str.data, "%ld", data) - str.data;

    ngx_crc32_init(hash);
    ngx_crc32_update(&hash, name->data, name->len);
    ngx_crc32_update(&hash, config->data, config->len);
    ngx_crc32_update(&hash, str.data, str.len);
    ngx_crc32_final(hash);

    return hash;
}


ngx_int_t
ngx_proxy_wasm_load(ngx_proxy_wasm_filter_t *filter, ngx_log_t *log)
{
    dd("enter");

    if (filter->loaded) {
        dd("loaded");
        return NGX_OK;
    }

    if (filter->module == NULL) {
        filter->ecode = NGX_PROXY_WASM_ERR_BAD_HOST_INTERFACE;
        return NGX_ERROR;
    }

    filter->log = log;
    filter->name = &filter->module->name;
    filter->id = ngx_proxy_wasm_id(filter->name, &filter->config, filter->data);

    filter->node.key = filter->id;
    ngx_rbtree_insert(&ngx_proxy_wasm_filters_rbtree, &filter->node);

    if (ngx_proxy_wasm_init_abi(filter) != NGX_OK) {
        return NGX_ERROR;
    }

    filter->loaded = 1;

    dd("exit");

    return NGX_OK;
}


ngx_int_t
ngx_proxy_wasm_start(ngx_cycle_t *cycle)
{
    ngx_int_t                 rc;
    ngx_rbtree_node_t        *root, *sentinel, *node;
    ngx_proxy_wasm_filter_t  *filter;

    dd("enter");

    root = ngx_proxy_wasm_filters_rbtree.root;
    sentinel = ngx_proxy_wasm_filters_rbtree.sentinel;

    if (root == sentinel) {
        goto done;
    }

    for (node = ngx_rbtree_min(root, sentinel);
         node;
         node = ngx_rbtree_next(&ngx_proxy_wasm_filters_rbtree, node))
    {
        filter = ngx_rbtree_data(node, ngx_proxy_wasm_filter_t, node);

        rc = ngx_proxy_wasm_start_filter(filter);
        if (rc != NGX_OK) {
            ngx_proxy_wasm_log_error(NGX_LOG_EMERG, filter->log, filter->ecode,
                                     "failed initializing \"%V\" filter",
                                     filter->name);
            return NGX_ERROR;
        }
    }

done:

    return NGX_OK;
}


ngx_proxy_wasm_ctx_t *
ngx_proxy_wasm_ctx_alloc(ngx_pool_t *pool)
{
    ngx_proxy_wasm_ctx_t  *pwctx;

    pwctx = ngx_pcalloc(pool, sizeof(ngx_proxy_wasm_ctx_t));
    if (pwctx == NULL) {
        return NULL;
    }

    pwctx->pool = pool;

    ngx_rbtree_init(&pwctx->host_props_tree, &pwctx->host_props_sentinel,
                    ngx_str_rbtree_insert_value);

    return pwctx;
}


ngx_proxy_wasm_ctx_t *
ngx_proxy_wasm_ctx(ngx_uint_t *filter_ids, size_t nfilters,
    ngx_uint_t isolation, ngx_proxy_wasm_subsystem_t *subsys, void *data)
{
    size_t                      i;
    ngx_uint_t                  id;
    ngx_pool_t                 *pool;
    ngx_log_t                  *log;
    ngx_proxy_wasm_ctx_t       *pwctx;
    ngx_proxy_wasm_exec_t      *pwexec;
    ngx_proxy_wasm_filter_t    *filter;
    ngx_proxy_wasm_instance_t  *ictx;
    ngx_proxy_wasm_store_t     *pwstore = NULL;

    pwctx = subsys->get_context(data);
    if (pwctx == NULL) {
        return NULL;
    }

    if (!pwctx->ready) {
        pwctx->nfilters = nfilters;
        pwctx->isolation = isolation;

        ngx_log_debug2(NGX_LOG_DEBUG_WASM, pwctx->log, 0,
                       "proxy_wasm initializing filter chain "
                       "(nfilters: %l, isolation: %ui)",
                       pwctx->nfilters, pwctx->isolation);

        ngx_proxy_wasm_store_init(&pwctx->store, pwctx->pool);

        ngx_array_init(&pwctx->pwexecs, pwctx->pool, nfilters,
                       sizeof(ngx_proxy_wasm_exec_t));

        for (i = 0; i < nfilters; i++) {
            id = filter_ids[i];

            filter = ngx_proxy_wasm_filter_lookup(id);
            if (filter == NULL) {
                /* TODO: log error */
                ngx_wasm_assert(0);
                return NULL;
            }

            switch (pwctx->isolation) {
            case NGX_PROXY_WASM_ISOLATION_NONE:
                pwstore = filter->store;
                pool = filter->pool;
                log = pwctx->log;
                break;
            case NGX_PROXY_WASM_ISOLATION_STREAM:
                pwstore = &pwctx->store;
                pool = pwstore->pool;
                log = pwctx->log;
                break;
            case NGX_PROXY_WASM_ISOLATION_FILTER:
                pool = pwctx->pool;
                log = pwctx->log;
                break;
            default:
                ngx_proxy_wasm_log_error(NGX_LOG_WASM_NYI, pwctx->log, 0,
                                         "NYI - instance isolation: %d",
                                         pwctx->isolation);
                return NULL;
            }

            pwexec = ngx_array_push(&pwctx->pwexecs);
            if (pwexec == NULL) {
                return NULL;
            }

            ngx_memzero(pwexec, sizeof(ngx_proxy_wasm_exec_t));

            pwexec->index = i;
            pwexec->pool = pool;
            pwexec->filter = filter;
            pwexec->parent = pwctx;
            pwexec->log = ngx_pcalloc(pwexec->pool, sizeof(ngx_log_t));
            if (pwexec->log == NULL) {
                return NULL;
            }

            pwexec->log->file = log->file;
            pwexec->log->next = log->next;
            pwexec->log->writer = log->writer;
            pwexec->log->wdata = log->wdata;
            pwexec->log->log_level = log->log_level;
            pwexec->log->connection = log->connection;
            pwexec->log->handler = ngx_proxy_wasm_log_error_handler;
            pwexec->log->data = &pwexec->log_ctx;

            pwexec->log_ctx.pwexec = pwexec;
            pwexec->log_ctx.orig_log = log;

            ictx = ngx_proxy_wasm_get_instance(filter, pwstore, log);
            if (ictx == NULL) {
                return NULL;
            }

            pwexec->ictx = ictx;
            pwexec->id = ictx->next_id++;
            pwexec->root_id = filter->id;

            ngx_wasm_assert(pwexec->index + 1 == pwctx->pwexecs.nelts);

        }  /* for () */

        pwctx->ready = 1;

    }  /* !ready */

    return pwctx;
}


void
ngx_proxy_wasm_ctx_destroy(ngx_proxy_wasm_ctx_t *pwctx)
{
    size_t                      i;
    ngx_proxy_wasm_exec_t      *pwexec, *pwexecs;
    ngx_proxy_wasm_instance_t  *ictx;

    pwexecs = (ngx_proxy_wasm_exec_t *) pwctx->pwexecs.elts;

    for (i = 0; i < pwctx->pwexecs.nelts; i++) {
        pwexec = &pwexecs[i];
        ictx = pwexec->ictx;

        ngx_wasm_assert(pwexec->root_id != NGX_PROXY_WASM_ROOT_CTX_ID);

        ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, pwexec->log, 0,
                                 "\"%V\" filter freeing context #%d "
                                 "(%l/%l)",
                                 pwexec->filter->name, pwexec->id,
                                 pwexec->index + 1, pwctx->nfilters);

        if (pwexec->log) {
            if (pwexec->log_ctx.log_prefix.data) {
                ngx_pfree(pwexec->pool, pwexec->log_ctx.log_prefix.data);
            }

            ngx_pfree(pwexec->pool, pwexec->log);
        }

        ngx_proxy_wasm_release_instance(ictx, 0);
    }

    ngx_proxy_wasm_store_destroy(&pwctx->store);

    if (pwctx->authority.data) {
        ngx_pfree(pwctx->pool, pwctx->authority.data);
    }

    if (pwctx->scheme.data) {
        ngx_pfree(pwctx->pool, pwctx->scheme.data);
    }

    if (pwctx->path.data) {
        ngx_pfree(pwctx->pool, pwctx->path.data);
    }

    if (pwctx->start_time.data) {
        ngx_pfree(pwctx->pool, pwctx->start_time.data);
    }

    if (pwctx->upstream_address.data) {
        ngx_pfree(pwctx->pool, pwctx->upstream_address.data);
    }

    if (pwctx->upstream_port.data) {
        ngx_pfree(pwctx->pool, pwctx->upstream_port.data);
    }

    if (pwctx->connection_id.data) {
        ngx_pfree(pwctx->pool, pwctx->connection_id.data);
    }

    if (pwctx->mtls.data) {
        ngx_pfree(pwctx->pool, pwctx->mtls.data);
    }

    if (pwctx->root_id.data) {
        ngx_pfree(pwctx->pool, pwctx->root_id.data);
    }

    if (pwctx->call_status.data) {
        ngx_pfree(pwctx->pool, pwctx->call_status.data);
    }

    if (pwctx->response_status.data) {
        ngx_pfree(pwctx->pool, pwctx->response_status.data);
    }

    ngx_array_destroy(&pwctx->pwexecs);

    ngx_pfree(pwctx->pool, pwctx);
}


static ngx_int_t
ngx_proxy_wasm_action2rc(ngx_proxy_wasm_ctx_t *pwctx,
    ngx_proxy_wasm_exec_t *pwexec)
{
    ngx_int_t                 rc = NGX_ERROR;
    ngx_proxy_wasm_exec_t    *pwexecs;
    ngx_proxy_wasm_filter_t  *filter;
    ngx_proxy_wasm_action_e   action;

    filter = pwexec->filter;
    action = pwctx->action;

    dd("action: %d", action);

    if (pwexec->ecode) {
        if (!pwexec->ecode_logged
            && pwctx->step != NGX_PROXY_WASM_STEP_DONE)
        {
            ngx_proxy_wasm_log_error(NGX_LOG_WARN, pwexec->log,
                                     pwexec->ecode,
                                     "filter %l/%l failed resuming \"%V\" "
                                     "step in \"%V\" phase",
                                     pwexec->index + 1, pwctx->nfilters,
                                     ngx_proxy_wasm_step_name(pwctx->step),
                                     &pwctx->phase->name);

            pwexec->ecode_logged = 1;
        }
#if (NGX_DEBUG)
        else {
            if (pwexec->root_id == NGX_PROXY_WASM_ROOT_CTX_ID) {
                ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, pwexec->log,
                                         pwexec->ecode,
                                         "root context skipping \"%V\" "
                                         "step in \"%V\" phase",
                                         ngx_proxy_wasm_step_name(pwctx->step),
                                         &pwctx->phase->name);

            } else {
                ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, pwexec->log,
                                         pwexec->ecode,
                                         "filter %l/%l skipping \"%V\" "
                                         "step in \"%V\" phase",
                                         pwexec->index + 1, pwctx->nfilters,
                                         ngx_proxy_wasm_step_name(pwctx->step),
                                         &pwctx->phase->name);
            }
        }
#endif

        rc = filter->subsystem->ecode(pwexec->ecode);
        goto error;
    }

    /* exceptioned steps */

    switch (pwctx->step) {
    case NGX_PROXY_WASM_STEP_DONE:
        /* force-resume the done step */
        goto cont;
    default:
        break;
    }

    /* determine current action rc */

    switch (action) {
    case NGX_PROXY_WASM_ACTION_DONE:
        ngx_log_debug0(NGX_LOG_DEBUG_WASM, pwctx->log, 0, "proxy_wasm action "
                       "\"DONE\" -> \"CONTINUE\" to resume later phases");
        ngx_proxy_wasm_ctx_set_next_action(pwctx,
                                           NGX_PROXY_WASM_ACTION_CONTINUE);
        rc = NGX_DONE;
        goto done;

    case NGX_PROXY_WASM_ACTION_CONTINUE:
        goto cont;

    case NGX_PROXY_WASM_ACTION_PAUSE:
        switch (pwctx->phase->index) {
#ifdef NGX_WASM_HTTP
        case NGX_HTTP_REWRITE_PHASE:
        case NGX_HTTP_ACCESS_PHASE:
        case NGX_HTTP_CONTENT_PHASE:
            ngx_log_debug7(NGX_LOG_DEBUG_WASM, pwctx->log, 0,
                           "proxy_wasm pausing in \"%V\" phase "
                           "(root: %d, filter: %l/%l, step: %d, action: %d, "
                           "pwctx: %p)", &pwctx->phase->name,
                           pwexec->id == NGX_PROXY_WASM_ROOT_CTX_ID,
                           pwexec->index + 1, pwctx->nfilters,
                           pwctx->step, action, pwctx);
            goto yield;
#endif
        default:
            if (pwexec->root_id == NGX_PROXY_WASM_ROOT_CTX_ID) {
                ngx_proxy_wasm_log_error(NGX_LOG_ERR, pwexec->log,
                                         pwexec->ecode,
                                         "root context cannot pause in "
                                         "\"%V\" phase (\"%V\" step)",
                                         &pwctx->phase->name,
                                         ngx_proxy_wasm_step_name(pwctx->step));

            } else {
                ngx_proxy_wasm_log_error(NGX_LOG_ERR, pwexec->log,
                                         pwexec->ecode,
                                         "filter %l/%l cannot pause in "
                                         "\"%V\" phase (\"%V\" step)",
                                         pwexec->index + 1, pwctx->nfilters,
                                         &pwctx->phase->name,
                                         ngx_proxy_wasm_step_name(pwctx->step));
            }

            pwexecs = (ngx_proxy_wasm_exec_t *) pwctx->pwexecs.elts;
            pwexec = &pwexecs[pwctx->exec_index];
            pwexec->ecode = NGX_PROXY_WASM_ERR_NOT_YIELDABLE;
        }

        break;

    default:
        ngx_proxy_wasm_log_error(NGX_LOG_WASM_NYI, pwexec->log, 0,
                                 "NYI - proxy_wasm action: %d", action);
        goto error;

    }

error:

    ngx_wasm_assert(rc == NGX_ERROR
#ifdef NGX_WASM_HTTP
                    || rc >= NGX_HTTP_SPECIAL_RESPONSE
#endif
                    );

    if (rc == NGX_ERROR) {
        pwexec->ecode = NGX_PROXY_WASM_ERR_NOT_YIELDABLE;
    }

    goto done;

#ifdef NGX_WASM_HTTP
yield:

    rc = NGX_AGAIN;
    goto done;
#endif

cont:

    rc = NGX_OK;

done:

    return rc;
}


ngx_proxy_wasm_err_e
ngx_proxy_wasm_run_step(ngx_proxy_wasm_exec_t *pwexec,
    ngx_proxy_wasm_instance_t *ictx, ngx_proxy_wasm_step_e step)
{
    ngx_int_t                 rc;
    ngx_proxy_wasm_err_e      ecode;
    ngx_proxy_wasm_action_e   action = NGX_PROXY_WASM_ACTION_CONTINUE;
    ngx_proxy_wasm_ctx_t     *pwctx = pwexec->parent;
    ngx_proxy_wasm_filter_t  *filter = pwexec->filter;
    ngx_wavm_instance_t      *instance = ictx->instance;
#if (NGX_DEBUG)
    ngx_proxy_wasm_action_e   old_action = pwctx->action;
#endif

    ngx_wasm_assert(ictx->module == filter->module);
    ngx_wasm_assert(pwctx->phase);

    ictx->pwexec = pwexec;  /* set instance current pwexec */
    pwexec->ictx = ictx;    /* link pwexec to instance */

    /*
     * update instance log
     * (instance->data = ictx already set by instance_new)
     */
    ngx_wavm_instance_set_data(instance, ictx,
                               pwexec ? pwexec->log : filter->log);

    if (pwexec->root_id == NGX_PROXY_WASM_ROOT_CTX_ID) {
        ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, pwexec->log, 0,
                                 "root context resuming \"%V\" step "
                                 "in \"%V\" phase",
                                 ngx_proxy_wasm_step_name(step),
                                 &pwctx->phase->name);

    } else {
        ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, pwexec->log, 0,
                                 "filter %l/%l resuming \"%V\" step "
                                 "in \"%V\" phase",
                                 pwexec->index + 1, pwctx->nfilters,
                                 ngx_proxy_wasm_step_name(step),
                                 &pwctx->phase->name);
    }

    switch (step) {
    case NGX_PROXY_WASM_STEP_REQ_HEADERS:
    case NGX_PROXY_WASM_STEP_REQ_BODY:
    case NGX_PROXY_WASM_STEP_RESP_HEADERS:
    case NGX_PROXY_WASM_STEP_RESP_BODY:
#ifdef NGX_WASM_RESPONSE_TRAILERS
    case NGX_PROXY_WASM_STEP_RESP_TRAILERS:
#endif
        ecode = ngx_proxy_wasm_on_start(ictx, filter, 0);
        if (ecode != NGX_PROXY_WASM_ERR_NONE) {
            pwexec->ecode = ecode;
            goto done;
        }

        rc = filter->subsystem->resume(pwexec, step, &action);
        break;
    case NGX_PROXY_WASM_STEP_LOG:
        ngx_proxy_wasm_on_log(pwexec);
        rc = NGX_OK;
        break;
    case NGX_PROXY_WASM_STEP_DONE:
        ngx_proxy_wasm_on_done(pwexec);
        rc = NGX_OK;
        break;
    case NGX_PROXY_WASM_STEP_DISPATCH_RESPONSE:
        rc = filter->subsystem->resume(pwexec, step, &action);
        break;
    case NGX_PROXY_WASM_STEP_TICK:
        rc = ngx_proxy_wasm_on_tick(pwexec);
        break;
    default:
        ngx_proxy_wasm_log_error(NGX_LOG_WASM_NYI, pwexec->log, 0,
                                 "NYI - proxy_wasm step: %d", step);
        rc = NGX_ERROR;
        break;
    }

    dd("rc: %ld, old_action: %d, pwctx->action: %d, ret action: %d",
       rc, old_action, pwctx->action, action);

    /* pwctx->action writes in host calls overwrite action return value */

    if (pwctx->action != action) {
        switch (pwctx->action) {
        case NGX_PROXY_WASM_ACTION_DONE:
            /* e.g. proxy_send_local_response() */
            ngx_wasm_assert(pwctx->action != old_action);
            goto done;
        default:
            ngx_proxy_wasm_ctx_set_next_action(pwctx, action);
            break;
        }
    }

    ngx_wasm_assert(rc == NGX_OK
                    || rc == NGX_ABORT
                    || rc == NGX_ERROR);

    if (rc == NGX_OK) {
        pwexec->ecode = NGX_PROXY_WASM_ERR_NONE;

    } else if (rc == NGX_ABORT) {
        pwexec->ecode = NGX_PROXY_WASM_ERR_INSTANCE_TRAPPED;

    } else if (rc == NGX_ERROR) {
        pwexec->ecode = NGX_PROXY_WASM_ERR_UNKNOWN;
    }

done:

    ictx->pwexec = NULL;

    return pwexec->ecode;
}


ngx_int_t
ngx_proxy_wasm_resume(ngx_proxy_wasm_ctx_t *pwctx,
    ngx_wasm_phase_t *phase, ngx_proxy_wasm_step_e step)
{
    size_t                      i;
    ngx_int_t                   rc = NGX_OK;
    ngx_proxy_wasm_filter_t    *filter;
    ngx_proxy_wasm_instance_t  *ictx;
    ngx_proxy_wasm_exec_t      *pwexec, *pwexecs;

    dd("enter");

    switch (step) {
    case NGX_PROXY_WASM_STEP_TICK:
    case NGX_PROXY_WASM_STEP_DONE:
    case NGX_PROXY_WASM_STEP_RESP_BODY:
    case NGX_PROXY_WASM_STEP_RESP_HEADERS:
    case NGX_PROXY_WASM_STEP_DISPATCH_RESPONSE:
        break;
    default:
        if (step <= pwctx->last_step) {
            dd("step %d already completed, exit", step);
            ngx_wasm_assert(rc == NGX_OK);
            goto ret;
        }
    }

    pwctx->step = step;

    /* resume filters chain */

    pwexecs = (ngx_proxy_wasm_exec_t *) pwctx->pwexecs.elts;

    for (i = pwctx->exec_index; i < pwctx->pwexecs.nelts; i++) {
        dd("exec_index: %ld", i);

        pwexec = &pwexecs[i];
        filter = pwexec->filter;
        ictx = pwexec->ictx;

        ngx_wasm_assert(pwexec->root_id != NGX_PROXY_WASM_ROOT_CTX_ID);

        /* check for trap */

        if (ictx->instance->trapped && !pwexec->ecode) {
            pwexec->ecode = NGX_PROXY_WASM_ERR_INSTANCE_TRAPPED;
        }

        /* check for yielded state */

        rc = ngx_proxy_wasm_action2rc(pwctx, pwexec);
        if (rc != NGX_OK) {
            goto ret;
        }

        /* run step */

        pwexec->ecode = ngx_proxy_wasm_run_step(pwexec, ictx, step);
        dd("pwexec->ecode: %d, pwctx->action: %d (pwctx: %p)",
           pwexec->ecode, pwctx->action, pwctx);
        if (pwexec->ecode != NGX_PROXY_WASM_ERR_NONE) {
            rc = filter->subsystem->ecode(pwexec->ecode);
            goto ret;
        }

        /* check for yield/done */

        rc = ngx_proxy_wasm_action2rc(pwctx, pwexec);
        if (rc != NGX_OK) {
            if (rc == NGX_AGAIN
                && pwctx->exec_index + 1 <= pwctx->nfilters)
            {
                dd("yield: resume on next filter "
                   "(idx: %ld -> %ld, nelts: %ld)",
                   pwctx->exec_index, pwctx->exec_index + 1,
                   pwctx->pwexecs.nelts);

                pwctx->exec_index++;
            }

            goto ret;
        }

        pwctx->exec_index++;

        dd("-------- next filter --------");
    }

    ngx_wasm_assert(rc == NGX_OK);

    /* next step */

    pwctx->last_step = pwctx->step;
    pwctx->exec_index = 0;

ret:

    if (step == NGX_PROXY_WASM_STEP_DONE) {
        ngx_proxy_wasm_ctx_destroy(pwctx);
    }

    dd("exit rc: %ld", rc);

    return rc;
}


ngx_wavm_ptr_t
ngx_proxy_wasm_alloc(ngx_proxy_wasm_exec_t *pwexec, size_t size)
{
    ngx_wavm_ptr_t            p;
    ngx_int_t                 rc;
    wasm_val_vec_t           *rets;
    ngx_proxy_wasm_filter_t  *filter = pwexec->filter;
    ngx_wavm_instance_t      *instance = ngx_proxy_wasm_pwexec2instance(pwexec);

    rc = ngx_wavm_instance_call_funcref(instance,
                                        filter->proxy_on_memory_allocate,
                                        &rets, size);
    if (rc != NGX_OK) {
        ngx_proxy_wasm_log_error(NGX_LOG_CRIT, pwexec->log, 0,
                                 "proxy_wasm_alloc(%uz) failed", size);
        return 0;
    }

    p = rets->data[0].of.i32;

    ngx_log_debug3(NGX_LOG_DEBUG_WASM, pwexec->log, 0,
                   "proxy_wasm_alloc: %uz:%uz:%uz",
                   ngx_wavm_memory_data_size(instance->memory), p, size);

    return p;
}


void
ngx_proxy_wasm_store_destroy(ngx_proxy_wasm_store_t *store)
{
    ngx_queue_t                *q;
    ngx_proxy_wasm_instance_t  *ictx;

    dd("enter");

    while (!ngx_queue_empty(&store->busy)) {
        dd("remove busy");
        q = ngx_queue_head(&store->busy);
        ictx = ngx_queue_data(q, ngx_proxy_wasm_instance_t, q);

        ngx_queue_remove(&ictx->q);
        ngx_queue_insert_tail(&store->free, &ictx->q);
    }

    while (!ngx_queue_empty(&store->free)) {
        q = ngx_queue_head(&store->free);
        ictx = ngx_queue_data(q, ngx_proxy_wasm_instance_t, q);

        dd("remove free");
        ngx_proxy_wasm_release_instance(ictx, 1);
    }

    while (!ngx_queue_empty(&store->sweep)) {
        dd("remove sweep");
        q = ngx_queue_head(&store->sweep);
        ictx = ngx_queue_data(q, ngx_proxy_wasm_instance_t, q);

        ngx_queue_remove(&ictx->q);
        ngx_proxy_wasm_instance_destroy(ictx);
    }

    dd("exit");
}


/* static */


static ngx_inline ngx_wavm_funcref_t *
ngx_proxy_wasm_lookup_func(ngx_proxy_wasm_filter_t *filter, const char *n)
{
    ngx_str_t   name;

    name.data = (u_char *) n;
    name.len = ngx_strlen(n);

    return ngx_wavm_module_func_lookup(filter->module, &name);
}


static ngx_proxy_wasm_abi_version_e
ngx_proxy_wasm_abi_version(ngx_proxy_wasm_filter_t *filter)
{
    size_t                    i;
    ngx_wavm_module_t        *module = filter->module;
    const wasm_exporttype_t  *exporttype;
    const wasm_name_t        *exportname;

    for (i = 0; i < module->exports.size; i++) {
        exporttype = ((wasm_exporttype_t **) module->exports.data)[i];
        exportname = wasm_exporttype_name(exporttype);

        if (ngx_str_eq(exportname->data, exportname->size,
                       "proxy_abi_version_0_2_1", -1))
        {
            return NGX_PROXY_WASM_0_2_1;
        }

        if (ngx_str_eq(exportname->data, exportname->size,
                       "proxy_abi_version_0_2_0", -1))
        {
            return NGX_PROXY_WASM_0_2_0;
        }

        if (ngx_str_eq(exportname->data, exportname->size,
                       "proxy_abi_version_0_1_0", -1))
        {
            return NGX_PROXY_WASM_0_1_0;
        }

#if (NGX_DEBUG)
        if (ngx_str_eq(exportname->data, exportname->size,
                       "proxy_abi_version_vnext", -1))
        {
            return NGX_PROXY_WASM_VNEXT;
        }
#endif
    }

    return NGX_PROXY_WASM_UNKNOWN;
}


static ngx_int_t
ngx_proxy_wasm_init_abi(ngx_proxy_wasm_filter_t *filter)
{
    ngx_log_debug4(NGX_LOG_DEBUG_WASM, filter->log, 0,
                   "proxy_wasm initializing \"%V\" filter "
                   "(config size: %d, filter: %p, filter->id: %ui)",
                   filter->name, filter->config.len, filter, filter->id);

    filter->abi_version = ngx_proxy_wasm_abi_version(filter);

    switch (filter->abi_version) {
    case NGX_PROXY_WASM_0_1_0:
    case NGX_PROXY_WASM_0_2_0:
    case NGX_PROXY_WASM_0_2_1:
        break;
    case NGX_PROXY_WASM_UNKNOWN:
        filter->ecode = NGX_PROXY_WASM_ERR_UNKNOWN_ABI;
        break;
    default:
        filter->ecode = NGX_PROXY_WASM_ERR_BAD_ABI;
        break;
    }

    if (filter->ecode) {
        ngx_proxy_wasm_log_error(NGX_LOG_EMERG, filter->log, filter->ecode,
                                 "failed loading \"%V\" filter",
                                 filter->name);
        return NGX_ERROR;
    }

    /* malloc */

    filter->proxy_on_memory_allocate =
        ngx_proxy_wasm_lookup_func(filter, "malloc");

    if (filter->proxy_on_memory_allocate == NULL) {
        filter->proxy_on_memory_allocate =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_memory_allocate");

        if (filter->proxy_on_memory_allocate == NULL) {
            filter->ecode = NGX_PROXY_WASM_ERR_BAD_MODULE_INTERFACE;

            ngx_proxy_wasm_log_error(NGX_LOG_EMERG, filter->log, filter->ecode,
                                     "\"%V\" filter missing malloc",
                                     filter->name);
            return NGX_ERROR;
        }
    }

    /* context */

    filter->proxy_on_context_create =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_context_create");
    filter->proxy_on_context_finalize =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_context_finalize");

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.1.0 - 0.2.1 */
        filter->proxy_on_done =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_done");
        filter->proxy_on_log =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_log");
        filter->proxy_on_context_finalize =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_delete");
    }

    /* configuration */

    filter->proxy_on_vm_start =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_vm_start");
    filter->proxy_on_plugin_start =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_plugin_start");

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.1.0 - 0.2.1 */
        filter->proxy_on_plugin_start =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_configure");
    }

    /* stream */

    filter->proxy_on_new_connection =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_new_connection");
    filter->proxy_on_downstream_data =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_downstream_data");
    filter->proxy_on_upstream_data =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_upstream_data");
    filter->proxy_on_downstream_close =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_downstream_close");
    filter->proxy_on_upstream_close =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_upstream_close");

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.1.0 - 0.2.1 */
        filter->proxy_on_downstream_close =
            ngx_proxy_wasm_lookup_func(
                filter, "proxy_on_downstream_connection_close");
        filter->proxy_on_upstream_close =
            ngx_proxy_wasm_lookup_func(
                filter, "proxy_on_upstream_connection_close");
    }

    /* http */

    filter->proxy_on_http_request_headers =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_request_headers");
    filter->proxy_on_http_request_body =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_request_body");
    filter->proxy_on_http_request_trailers =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_request_trailers");
    filter->proxy_on_http_request_metadata =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_request_metadata");
    filter->proxy_on_http_response_headers =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_response_headers");
    filter->proxy_on_http_response_body =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_response_body");
    filter->proxy_on_http_response_trailers =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_response_trailers");
    filter->proxy_on_http_response_metadata =
        ngx_proxy_wasm_lookup_func(filter,
                                   "proxy_on_http_response_metadata");

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.1.0 - 0.2.1 */
        filter->proxy_on_http_request_headers =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_request_headers");
        filter->proxy_on_http_request_body =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_request_body");
        filter->proxy_on_http_request_trailers =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_request_trailers");
        filter->proxy_on_http_request_metadata =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_request_metadata");
        filter->proxy_on_http_response_headers =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_response_headers");
        filter->proxy_on_http_response_body =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_response_body");
        filter->proxy_on_http_response_trailers =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_response_trailers");
        filter->proxy_on_http_response_metadata =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_response_metadata");
    }

    /* shared queue */

    filter->proxy_on_queue_ready =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_queue_ready");

    /* timers */

    filter->proxy_create_timer =
        ngx_proxy_wasm_lookup_func(filter, "proxy_create_timer");
    filter->proxy_delete_timer =
        ngx_proxy_wasm_lookup_func(filter, "proxy_delete_timer");
    filter->proxy_on_timer_ready =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_timer_ready");

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.1.0 - 0.2.1 */
        filter->proxy_on_timer_ready =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_tick");
    }

    /* http callouts */

    filter->proxy_on_http_call_response =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_http_call_response");

    /* grpc callouts */

    filter->proxy_on_grpc_call_response_header_metadata =
        ngx_proxy_wasm_lookup_func(
            filter, "proxy_on_grpc_call_response_header_metadata");
    filter->proxy_on_grpc_call_response_message =
        ngx_proxy_wasm_lookup_func(
            filter, "proxy_on_grpc_call_response_message");
    filter->proxy_on_grpc_call_response_trailer_metadata =
        ngx_proxy_wasm_lookup_func(
            filter, "proxy_on_grpc_call_response_trailer_metadata");
    filter->proxy_on_grpc_call_close =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_grpc_call_close");

    /* custom extensions */

    filter->proxy_on_custom_callback =
        ngx_proxy_wasm_lookup_func(filter, "proxy_on_custom_callback");

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.2.0 - 0.2.1 */
        filter->proxy_on_custom_callback =
            ngx_proxy_wasm_lookup_func(filter, "proxy_on_foreign_function");
    }

    /* validate */

    if (filter->proxy_on_context_create == NULL
        || filter->proxy_on_vm_start == NULL
        || filter->proxy_on_plugin_start == NULL)
    {
        filter->ecode = NGX_PROXY_WASM_ERR_BAD_MODULE_INTERFACE;

        ngx_proxy_wasm_log_error(NGX_LOG_EMERG, filter->log, filter->ecode,
                                 "\"%V\" filter missing one of: "
                                 "on_context_create, on_vm_start, "
                                 "on_plugin_start", filter->name);
        return NGX_ERROR;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_proxy_wasm_start_filter(ngx_proxy_wasm_filter_t *filter)
{
    ngx_proxy_wasm_err_e        ecode;
    ngx_proxy_wasm_instance_t  *ictx;

    ngx_wasm_assert(filter->loaded);

    if (filter->ecode) {
        return NGX_ERROR;
    }

    if (filter->started) {
        return NGX_OK;
    }

    ictx = ngx_proxy_wasm_get_instance(filter, filter->store, filter->log);
    if (ictx == NULL) {
        return NGX_ERROR;
    }

    /*
     * update instance log
     * FFI-injected filters have a valid log while the instance's
     * might be outdated.
     */
    ngx_wavm_instance_set_data(ictx->instance, ictx, filter->log);

    ecode = ngx_proxy_wasm_on_start(ictx, filter, 1);
    if (ecode != NGX_PROXY_WASM_ERR_NONE) {
        filter->ecode = ecode;
        return NGX_ERROR;
    }

    filter->started = 1;

    return NGX_OK;
}


ngx_proxy_wasm_instance_t *
ngx_proxy_wasm_get_instance(ngx_proxy_wasm_filter_t *filter,
    ngx_proxy_wasm_store_t *store, ngx_log_t *log)
{
    ngx_queue_t                *q;
    ngx_pool_t                 *pool;
    ngx_wavm_module_t          *module = filter->module;
    ngx_proxy_wasm_instance_t  *ictx = NULL;

    if (store == NULL) {
        dd("no store, jump to create");
        pool = filter->pool;
        goto create;
    }

    pool = store->pool;

    dd("lookup instance in store: %p", store);

    for (q = ngx_queue_head(&store->busy);
         q != ngx_queue_sentinel(&store->busy);
         q = ngx_queue_next(q))
    {
        ictx = ngx_queue_data(q, ngx_proxy_wasm_instance_t, q);

        if (ictx->instance->trapped) {
            ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, log, 0,
                                     "\"%V\" filter freeing trapped instance "
                                     "(ictx: %p, store: %p)",
                                     filter->name, ictx, store);
            q = ngx_queue_next(&ictx->q);
            ngx_proxy_wasm_release_instance(ictx, 1);
            continue;
        }

        if (ictx->module == filter->module) {
            dd("reuse instance");
            ngx_wasm_assert(ictx->nrefs);
            goto reuse;
        }
    }

    for (q = ngx_queue_head(&store->free);
         q != ngx_queue_sentinel(&store->free);
         q = ngx_queue_next(q))
    {
        ictx = ngx_queue_data(q, ngx_proxy_wasm_instance_t, q);

        if (ictx->module == filter->module) {
            ngx_wasm_assert(ictx->nrefs == 0);
            ngx_queue_remove(&ictx->q);
            goto reuse;
        }
    }

    dd("create instance in store: %p", store);

create:

    ictx = ngx_pcalloc(pool, sizeof(ngx_proxy_wasm_instance_t));
    if (ictx == NULL) {
        goto error;
    }

    ictx->next_id = 1;
    ictx->pool = pool;
    ictx->log = log;
    ictx->store = store;
    ictx->module = module;

    ngx_rbtree_init(&ictx->root_ctxs, &ictx->sentinel_root_ctxs,
                    ngx_rbtree_insert_value);

    ngx_rbtree_init(&ictx->tree_ctxs, &ictx->sentinel_ctxs,
                    ngx_rbtree_insert_value);

    ictx->instance = ngx_wavm_instance_create(module, pool, log, ictx);
    if (ictx->instance == NULL) {
        ngx_pfree(pool, ictx);
        goto error;
    }

    ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, log, 0,
                             "\"%V\" filter new instance (ictx: %p, store: %p)",
                             filter->name, ictx, store);

    goto done;

error:

    return NULL;

reuse:

    ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, log, 0,
                             "\"%V\" filter reusing instance "
                             "(ictx: %p, nrefs: %d, store: %p)",
                             filter->name, ictx, ictx->nrefs + 1, store);

done:

    if (store && !ictx->nrefs) {
        ngx_queue_insert_tail(&store->busy, &ictx->q);
    }

    ictx->nrefs++;

    return ictx;
}


void
ngx_proxy_wasm_release_instance(ngx_proxy_wasm_instance_t *ictx,
    unsigned sweep)
{
    if (sweep) {
        ictx->nrefs = 0;

    } else if (ictx->nrefs) {
        ictx->nrefs--;
    }

    dd("ictx: %p (nrefs: %ld, sweep: %d)",
       ictx, ictx->nrefs, sweep);

    if (ictx->nrefs == 0) {
        if (ictx->store) {
            dd("remove from busy");
            ngx_queue_remove(&ictx->q); /* remove from busy/free */

            if (sweep) {
                dd("insert in sweep");
                ngx_queue_insert_tail(&ictx->store->sweep, &ictx->q);

            } else {
                dd("insert in free");
                ngx_queue_insert_tail(&ictx->store->free, &ictx->q);
            }

        } else {
            dd("destroy");
            ngx_proxy_wasm_instance_destroy(ictx);
        }
    }

    dd("exit");
}


static void
ngx_proxy_wasm_instance_destroy(ngx_proxy_wasm_instance_t *ictx)
{
    ngx_rbtree_node_t      **root, **sentinel, *s, *n;
    ngx_proxy_wasm_exec_t   *rexec;

    dd("enter");

    root = &ictx->root_ctxs.root;
    s = &ictx->sentinel_root_ctxs;
    sentinel = &s;

    while (*root != *sentinel) {
        n = ngx_rbtree_min(*root, *sentinel);
        rexec = ngx_rbtree_data(n, ngx_proxy_wasm_exec_t, node);

        if (rexec->ev) {
            ngx_del_timer(rexec->ev);
            ngx_free(rexec->ev);

            rexec->ev = NULL;
        }

        if (rexec->log) {
            ngx_pfree(rexec->pool, rexec->log);
        }

        if (rexec->parent) {
            if (rexec->log_ctx.log_prefix.data) {
                ngx_pfree(rexec->pool, rexec->log_ctx.log_prefix.data);
            }

            ngx_pfree(rexec->pool, rexec->parent);
        }

        ngx_rbtree_delete(&ictx->root_ctxs, n);

        ngx_pfree(rexec->pool, rexec);
    }

    ngx_wavm_instance_destroy(ictx->instance);

    ngx_pfree(ictx->pool, ictx);

    dd("exit");
}


static ngx_proxy_wasm_err_e
ngx_proxy_wasm_on_start(ngx_proxy_wasm_instance_t *ictx,
    ngx_proxy_wasm_filter_t *filter, unsigned start)
{
    ngx_int_t               rc;
    ngx_log_t              *log;
    ngx_proxy_wasm_exec_t  *rexec, *pwexec = ictx->pwexec;
    ngx_wavm_instance_t    *instance = ictx->instance;
    wasm_val_vec_t         *rets;

    rexec = ngx_proxy_wasm_root_lookup(ictx, filter->id);
    if (rexec == NULL) {
        rexec = ngx_pcalloc(ictx->pool, sizeof(ngx_proxy_wasm_exec_t));
        if (rexec == NULL) {
            return NGX_PROXY_WASM_ERR_START_FAILED;
        }

        rexec->root_id = NGX_PROXY_WASM_ROOT_CTX_ID;
        rexec->id = filter->id;
        rexec->pool = ictx->pool;
        rexec->log = filter->log;
        rexec->filter = filter;
        rexec->ictx = ictx;

        log = filter->log;

        rexec->log = ngx_pcalloc(rexec->pool, sizeof(ngx_log_t));
        if (rexec->log == NULL) {
            return NGX_PROXY_WASM_ERR_START_FAILED;
        }

        rexec->log->file = log->file;
        rexec->log->next = log->next;
        rexec->log->writer = log->writer;
        rexec->log->wdata = log->wdata;
        rexec->log->log_level = log->log_level;
        rexec->log->handler = ngx_proxy_wasm_log_error_handler;
        rexec->log->data = &rexec->log_ctx;

        rexec->log_ctx.pwexec = rexec;
        rexec->log_ctx.orig_log = log;

        rexec->parent = ngx_pcalloc(rexec->pool, sizeof(ngx_proxy_wasm_ctx_t));
        if (rexec->parent == NULL) {
            return NGX_PROXY_WASM_ERR_START_FAILED;
        }

        rexec->parent->id = NGX_PROXY_WASM_ROOT_CTX_ID;
        rexec->parent->pool = rexec->pool;
        rexec->parent->log = rexec->log;

        rexec->node.key = rexec->id;
        ngx_rbtree_insert(&ictx->root_ctxs, &rexec->node);
    }

    ictx->pwexec = rexec;  /* set instance current pwexec */

    if (!rexec->started) {
        dd("start root exec ctx (rexec: %p, root_id: %ld, id: %ld, ictx: %p)",
           rexec, rexec->root_id, rexec->id, ictx);

        rc = ngx_wavm_instance_call_funcref(instance,
                                            filter->proxy_on_context_create,
                                            NULL,
                                            rexec->id, rexec->root_id);
        if (rc != NGX_OK) {
            return NGX_PROXY_WASM_ERR_START_FAILED;
        }

        if (start) {
            rc = ngx_wavm_instance_call_funcref(instance,
                                                filter->proxy_on_vm_start,
                                                &rets,
                                                rexec->id, 0);
            if (rc != NGX_OK || !rets->data[0].of.i32) {
                return NGX_PROXY_WASM_ERR_START_FAILED;
            }
        }

        rc = ngx_wavm_instance_call_funcref(instance,
                                            filter->proxy_on_plugin_start,
                                            &rets,
                                            rexec->id, filter->config.len);
        if (rc != NGX_OK || !rets->data[0].of.i32) {
            return NGX_PROXY_WASM_ERR_START_FAILED;
        }

        rexec->started = 1;
    }

    ictx->pwexec = pwexec;  /* set instance current pwexec */

    if (pwexec && pwexec->root_id != NGX_PROXY_WASM_ROOT_CTX_ID
        && ngx_proxy_wasm_ctx_lookup(ictx, pwexec->id) == NULL)
    {
        dd("start exec ctx (pwexec: %p, root_id: %ld, id: %ld, ictx: %p)",
           pwexec, pwexec->root_id, pwexec->id, ictx);

        rc = ngx_wavm_instance_call_funcref(instance,
                                            filter->proxy_on_context_create,
                                            NULL,
                                            pwexec->id, pwexec->root_id);
        if (rc != NGX_OK) {
            return NGX_PROXY_WASM_ERR_INSTANCE_FAILED;
        }

        pwexec->node.key = pwexec->id;
        ngx_rbtree_insert(&ictx->tree_ctxs, &pwexec->node);
    }

    return NGX_PROXY_WASM_ERR_NONE;
}


static void
ngx_proxy_wasm_on_log(ngx_proxy_wasm_exec_t *pwexec)
{
    ngx_proxy_wasm_filter_t  *filter = pwexec->filter;
    ngx_wavm_instance_t      *instance = ngx_proxy_wasm_pwexec2instance(pwexec);

    if (filter->abi_version < NGX_PROXY_WASM_VNEXT) {
        /* 0.1.0 - 0.2.1 */
        (void) ngx_wavm_instance_call_funcref(instance, filter->proxy_on_done,
                                              NULL, pwexec->id);
    }

    (void) ngx_wavm_instance_call_funcref(instance, filter->proxy_on_log,
                                          NULL, pwexec->id);
}


static void
ngx_proxy_wasm_on_done(ngx_proxy_wasm_exec_t *pwexec)
{
    ngx_wavm_instance_t             *instance;
    ngx_proxy_wasm_filter_t         *filter = pwexec->filter;
#if 1
#ifdef NGX_WASM_HTTP
    ngx_http_proxy_wasm_dispatch_t  *call;
#endif
#endif

    instance = ngx_proxy_wasm_pwexec2instance(pwexec);

    ngx_proxy_wasm_log_error(NGX_LOG_DEBUG, pwexec->log, 0,
                             "filter %l/%l finalizing context",
                             pwexec->index + 1, pwexec->parent->nfilters);

#if 1
#ifdef NGX_WASM_HTTP
    call = pwexec->call;
    if (call) {
        ngx_log_debug3(NGX_LOG_DEBUG_WASM, pwexec->log, 0,
                       "proxy_wasm \"%V\" filter (%l/%l) "
                       "cancelling HTTP dispatch",
                       pwexec->filter->name, pwexec->index + 1,
                       pwexec->parent->nfilters);

        ngx_http_proxy_wasm_dispatch_destroy(call);

        pwexec->call = NULL;
    }
#endif
#endif

    (void) ngx_wavm_instance_call_funcref(instance,
                                          filter->proxy_on_context_finalize,
                                          NULL, pwexec->id);

    ngx_rbtree_delete(&pwexec->ictx->tree_ctxs, &pwexec->node);
}


static ngx_int_t
ngx_proxy_wasm_on_tick(ngx_proxy_wasm_exec_t *pwexec)
{
    ngx_int_t                 rc;
    wasm_val_vec_t            args;
    ngx_proxy_wasm_filter_t  *filter = pwexec->filter;

    ngx_wasm_assert(pwexec->root_id == NGX_PROXY_WASM_ROOT_CTX_ID);

    wasm_val_vec_new_uninitialized(&args, 1);
    ngx_wasm_vec_set_i32(&args, 0, pwexec->id);

    rc = ngx_wavm_instance_call_funcref_vec(pwexec->ictx->instance,
                                            filter->proxy_on_timer_ready,
                                            NULL, &args);

    wasm_val_vec_delete(&args);

    return rc;
}
