#ifndef DDEBUG
#define DDEBUG 0
#endif
#include "ddebug.h"

#include <ngx_wasm.h>


#if 0
static void
ngx_wasm_chain_log_debug(ngx_log_t *log, ngx_chain_t *in, char *fmt)
{
#if (NGX_DEBUG)
    size_t        len;
    ngx_chain_t  *cl;
    ngx_buf_t    *buf;
    ngx_str_t     s;

    cl = in;

    while (cl) {
        buf = cl->buf;
        len = buf->last - buf->pos;

        s.len = len;
        s.data = buf->pos;

        ngx_log_debug7(NGX_LOG_DEBUG_WASM, log, 0,
                       "%s: \"%V\" (buf: %p, len: %d, last_buf: %d,"
                       " last_in_chain: %d, flush: %d)",
                       fmt, &s, buf, len, buf->last_buf,
                       buf->last_in_chain, buf->flush);

        cl = cl->next;
    }
#endif
}
#endif


size_t
ngx_wasm_chain_len(ngx_chain_t *in, unsigned *eof)
{
    size_t        len = 0;
    ngx_buf_t    *buf;
    ngx_chain_t  *cl;

    for (cl = in; cl; cl = cl->next) {
        buf = cl->buf;
        len += ngx_buf_size(buf);

        if (buf->last_buf || buf->last_in_chain) {
            if (eof) {
                *eof = 1;
            }

            break;
        }
    }

    return len;
}


ngx_uint_t
ngx_wasm_chain_clear(ngx_chain_t *in, size_t offset, unsigned *eof,
    unsigned *flush)
{
    size_t        pos = 0, len, n;
    ngx_uint_t    fill = 0;
    ngx_buf_t    *buf;
    ngx_chain_t  *cl;

    for (cl = in; cl; cl = cl->next) {
        buf = cl->buf;
        len = buf->last - buf->pos;

        if (eof && (buf->last_buf || buf->last_in_chain)) {
            *eof = 1;
        }

        if (flush && buf->flush) {
            *flush = 1;
        }

        if (offset && pos + len > offset) {
            /* reaching start offset */
            n = len - ((pos + len) - offset);  /* bytes left until offset */
            pos += n;
            buf->last = buf->pos + n;  /* partially consume buffer */

            ngx_wasm_assert(pos == offset);

        } else if (pos == offset) {
            /* past start offset, consume buffer */
            buf->pos = buf->last;

        } else {
            /* prior start offset, preserve buffer */
            pos += len;
        }

        buf->last_buf = 0;
        buf->last_in_chain = 0;
    }

    if (pos < offset) {
        fill = offset - pos;
        pos += fill;
    }

    ngx_wasm_assert(pos == offset);

    return fill;
}


ngx_chain_t *
ngx_wasm_chain_get_free_buf(ngx_pool_t *p, ngx_chain_t **free,
    size_t len, ngx_buf_tag_t tag, unsigned reuse)
{
    ngx_buf_t    *b;
    ngx_chain_t  *cl;
    u_char       *start, *end;

    if (reuse && *free) {
        cl = *free;
        *free = cl->next;
        cl->next = NULL;

        b = cl->buf;
        start = b->start;
        end = b->end;

        if (start && (size_t) (end - start) >= len) {
            ngx_log_debug4(NGX_LOG_DEBUG_WASM, p->log, 0,
                           "wasm reuse free buf memory %O >= %uz, cl:%p, p:%p",
                           (off_t) (end - start), len, cl, start);

            ngx_memzero(b, sizeof(ngx_buf_t));

            b->start = start;
            b->pos = start;
            b->last = start;
            b->end = end;
            b->tag = tag;

            if (len) {
                b->temporary = 1;
            }

            return cl;
        }

        ngx_log_debug4(NGX_LOG_DEBUG_WASM, p->log, 0,
                       "wasm reuse free buf chain, but reallocate memory "
                       "because %uz >= %O, cl:%p, p:%p", len,
                       (off_t) (b->end - b->start), cl, b->start);

        if (ngx_buf_in_memory(b) && b->start) {
            ngx_pfree(p, b->start);
        }

        ngx_memzero(b, sizeof(ngx_buf_t));

        if (len == 0) {
            return cl;
        }

        b->start = ngx_palloc(p, len);
        if (b->start == NULL) {
            return NULL;
        }

        b->end = b->start + len;
        b->pos = b->start;
        b->last = b->start;
        b->tag = tag;
        b->temporary = 1;

        return cl;
    }

    cl = ngx_alloc_chain_link(p);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf = len ? ngx_create_temp_buf(p, len) : ngx_calloc_buf(p);
    if (cl->buf == NULL) {
        return NULL;
    }

    cl->buf->tag = tag;
    cl->next = NULL;

    ngx_log_debug3(NGX_LOG_DEBUG_WASM, p->log, 0,
                   "wasm allocate new chainlink and new buf of size %uz, cl: %p"
                   ", buf: %p",
                   len, cl, cl->buf);

    return cl;
}


ngx_int_t
ngx_wasm_chain_prepend(ngx_pool_t *pool, ngx_chain_t **in,
    ngx_str_t *str, ngx_chain_t **free, ngx_buf_tag_t tag)
{
    ngx_chain_t  *first;
    ngx_buf_t    *buf;

    first = ngx_wasm_chain_get_free_buf(pool, free, str->len, tag, 1);
    if (first == NULL) {
        return NGX_ERROR;
    }

    buf = first->buf;

    buf->last = ngx_cpymem(buf->last, str->data, str->len);
    first->next = *in;

    *in = first;

    return NGX_OK;
}


ngx_int_t
ngx_wasm_chain_append(ngx_pool_t *pool, ngx_chain_t **in, size_t at,
    ngx_str_t *str, ngx_chain_t **free, ngx_buf_tag_t tag, unsigned extend)
{
    unsigned      eof = 0, flush = 0;
    ngx_uint_t    fill, rest;
    ngx_buf_t    *buf;
    ngx_chain_t  *cl, *nl, *ll = NULL;

    fill = ngx_wasm_chain_clear(*in, at, &eof, &flush);

    if (!extend) {
        fill = 0;
    }

    rest = str->len + fill;

    /* get tail */

    cl = *in;

    while (cl) {
        buf = cl->buf;

        if (ngx_buf_size(buf)) {
            ll = cl;
            cl = cl->next;
            continue;
        }

        /* zero size buf */

        if (buf->tag != tag) {
            if (ll) {
                /* ngx_wasm_assert(ll->next == cl); */
                ll->next = cl->next;
            }

            ngx_free_chain(pool, cl);
            cl = ll ? ll->next : NULL;
            continue;
        }

        buf->pos = buf->start;
        buf->last = buf->start;

        nl = cl;
        cl = cl->next;
        nl->next = *free;
        if (*free) {
            *free = nl;
        }
    }

    nl = ngx_wasm_chain_get_free_buf(pool, free, rest, tag, 1);
    if (nl == NULL) {
        return NGX_ERROR;
    }

    buf = nl->buf;

#if 1
    ngx_wasm_assert(!extend);
#else
    /* presently all calls have extend = 0 therefore fill == 0 */
    if (fill) {
        /* spillover fill */
        ngx_memset(buf->last, ' ', fill);
        buf->last += fill;
        rest -= fill;
    }
#endif

    /* write */

    ngx_wasm_assert(rest == str->len);

    buf->last = ngx_cpymem(buf->last, str->data, rest);

    if (flush) {
        buf->flush = 1;
    }

    if (eof) {
        buf->last_buf = 1;
        buf->last_in_chain = 1;
    }

    if (ll) {
        ll->next = nl;

    } else {
        *in = nl;
    }

    return NGX_OK;
}


ngx_int_t
ngx_wasm_bytes_from_path(wasm_byte_vec_t *out, u_char *path, ngx_log_t *log)
{
    ssize_t      n, fsize;
    u_char      *file_bytes = NULL;
    ngx_fd_t     fd;
    ngx_file_t   file;
    ngx_int_t    rc = NGX_ERROR;

    fd = ngx_open_file(path, NGX_FILE_RDONLY, NGX_FILE_OPEN, 0);
    if (fd == NGX_INVALID_FILE) {
        ngx_wasm_log_error(NGX_LOG_EMERG, log, ngx_errno,
                           ngx_open_file_n " \"%s\" failed",
                           path);
        return NGX_ERROR;
    }

    ngx_memzero(&file, sizeof(ngx_file_t));

    file.fd = fd;
    file.log = log;
    file.name.len = ngx_strlen(path);;
    file.name.data = path;

    if (ngx_fd_info(fd, &file.info) == NGX_FILE_ERROR) {
        ngx_wasm_log_error(NGX_LOG_EMERG, log, ngx_errno,
                           ngx_fd_info_n " \"%V\" failed", &file.name);
        goto close;
    }

    fsize = ngx_file_size(&file.info);

    file_bytes = ngx_alloc(fsize, log);
    if (file_bytes == NULL) {
        ngx_wasm_log_error(NGX_LOG_EMERG, log, 0,
                           "failed to allocate file_bytes for \"%V\"",
                           path);
        goto close;
    }

    n = ngx_read_file(&file, file_bytes, fsize, 0);
    if (n == NGX_ERROR) {
        ngx_wasm_log_error(NGX_LOG_EMERG, log, ngx_errno,
                           ngx_read_file_n " \"%V\" failed",
                           &file.name);
        goto close;
    }

    if (n != fsize) {
        ngx_wasm_log_error(NGX_LOG_EMERG, log, 0,
                           ngx_read_file_n " \"%V\" returned only "
                           "%z file_bytes instead of %uiz", &file.name,
                           n, fsize);
        goto close;
    }

    wasm_byte_vec_new(out, fsize, (const char *) file_bytes);

    rc = NGX_OK;

close:

    if (ngx_close_file(fd) == NGX_FILE_ERROR) {
        ngx_wasm_log_error(NGX_LOG_ERR, log, ngx_errno,
                           ngx_close_file_n " \"%V\" failed", &file.name);
    }

    if (file_bytes) {
        ngx_free(file_bytes);
    }

    return rc;
}


ngx_uint_t
ngx_wasm_list_nelts(ngx_list_t *list)
{
    ngx_uint_t        i, c = 0;
    ngx_list_part_t  *part;
    ngx_table_elt_t  *h;

    part = &list->part;

    while (part) {
        h = part->elts;

        for (i = 0; i < part->nelts; i++) {
            if (h[i].hash) {
                c++;
            }
        }

        part = part->next;
    }

    return c;
}


ngx_str_t *
ngx_wasm_get_list_elem(ngx_list_t *map, u_char *key, size_t key_len)
{
    size_t            i;
    ngx_table_elt_t  *elt;
    ngx_list_part_t  *part;

    part = &map->part;
    elt = part->elts;

    for (i = 0; /* void */; i++) {
        if (i >= part->nelts) {
            if (part->next == NULL) {
                break;
            }

            part = part->next;
            elt = part->elts;
            i = 0;
        }

#if 0
        dd("key: %.*s, value: %.*s",
           (int) elt[i].key.len, elt[i].key.data,
           (int) elt[i].value.len, elt[i].value.data);
#endif

        if (key_len == elt[i].key.len
            && ngx_strncasecmp(elt[i].key.data, key, key_len) == 0)
        {
            return &elt[i].value;
        }
    }

    return NULL;
}


ngx_msec_t
ngx_wasm_monotonic_time()
{
#if (NGX_HAVE_CLOCK_MONOTONIC)
    time_t            sec;
    ngx_uint_t        msec;
    struct timespec   ts;

#if defined(CLOCK_MONOTONIC_FAST)
    clock_gettime(CLOCK_MONOTONIC_FAST, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif

    sec = ts.tv_sec;
    msec = ts.tv_nsec / 1000000;

    return (ngx_msec_t) sec * 1000 + msec;
#else
    ngx_time_update();
    return ngx_current_msec;
#endif
}


void
ngx_wasm_wall_time(void *rtime)
{
    uint64_t     t;
    ngx_time_t  *tp;

    ngx_time_update();

    tp = ngx_timeofday();

    t = (tp->sec * 1000 + tp->msec) * 1e6;

    /* WASM might not align 64-bit integers to 8-byte boundaries. So we
     * need to buffer & copy here. */
    ngx_memcpy(rtime, &t, sizeof(uint64_t));
}


void
ngx_wasm_log_error(ngx_uint_t level, ngx_log_t *log, ngx_err_t err,
    const char *fmt, ...)
{
    va_list   args;
    u_char   *p, errstr[NGX_MAX_ERROR_STR];

    va_start(args, fmt);
    p = ngx_vsnprintf(errstr, NGX_MAX_ERROR_STR, fmt, args);
    va_end(args);

    ngx_log_error_core(level, log, err, "[wasm] %*s", p - errstr, errstr);
}
