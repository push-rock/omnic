#include "core/blockio.h"

#include <stdlib.h>
#include <string.h>

/* 分块流式 codec（snappy 64KB chunk 等）的小粒度 fread/fwrite 直落系统调用，
 * Windows 下单次调用开销可达 ~100µs。1MB 全缓冲把调用次数压缩两个数量级
 * （A7 实测 snappy 解压路径损耗 71% -> 个位数）。stdio 分配失败时静默回退
 * 默认缓冲，行为不变。 */
#define OMNIC_IO_BUF (1024u * 1024u)

void omnic_reader_init(omnic_reader *r, FILE *f) {
    r->f = f;
    r->mem = NULL;
    r->mem_len = r->mem_pos = 0;
    r->pend = NULL;
    r->pend_len = 0;
    r->pend_cap = 0;
    r->total_in = 0;
    (void)setvbuf(f, NULL, _IOFBF, OMNIC_IO_BUF);
}

void omnic_reader_init_mem(omnic_reader *r, const void *buf, size_t n) {
    r->f = NULL;
    r->mem = (const uint8_t *)buf;
    r->mem_len = n;
    r->mem_pos = 0;
    r->pend = NULL;
    r->pend_len = 0;
    r->pend_cap = 0;
    r->total_in = 0;
}

void omnic_reader_fini(omnic_reader *r) {
    free(r->pend);
    r->pend = NULL;
    r->pend_len = r->pend_cap = 0;
}

size_t omnic_reader_read(omnic_reader *r, void *buf, size_t want) {
    size_t got = 0;
    while (got < want && r->pend_len > 0) {
        size_t n = want - got;
        if (n > r->pend_len) n = r->pend_len;
        memcpy((char *)buf + got, r->pend, n);
        memmove(r->pend, r->pend + n, r->pend_len - n);
        r->pend_len -= n;
        got += n;
    }
    if (got < want) {
        if (r->mem != NULL) {
            size_t n = want - got;
            if (n > r->mem_len - r->mem_pos) n = r->mem_len - r->mem_pos;
            if (n > 0) {
                memcpy((char *)buf + got, r->mem + r->mem_pos, n);
                r->mem_pos += n;
                got += n;
            }
        } else {
            got += fread((char *)buf + got, 1, want - got, r->f);
        }
    }
    r->total_in += got;
    return got;
}

omnic_status omnic_reader_unread(omnic_reader *r, const void *buf, size_t n) {
    if (n == 0) return OMNIC_OK;
    if (r->pend_len + n > r->pend_cap) {
        size_t cap = (r->pend_len + n) * 2;
        uint8_t *p = (uint8_t *)realloc(r->pend, cap);
        if (p == NULL) return OMNIC_E_SYSTEM;
        r->pend = p;
        r->pend_cap = cap;
    }
    memmove(r->pend + n, r->pend, r->pend_len); /* 前插 */
    memcpy(r->pend, buf, n);
    r->pend_len += n;
    r->total_in -= n;
    return OMNIC_OK;
}

uint64_t omnic_reader_total(const omnic_reader *r) { return r->total_in; }

void omnic_writer_init(omnic_writer *w, FILE *f) {
    w->f = f;
    w->mem = NULL;
    w->mem_len = w->mem_cap = 0;
    w->total_out = 0;
    (void)setvbuf(f, NULL, _IOFBF, OMNIC_IO_BUF);
}

void omnic_writer_init_mem(omnic_writer *w) {
    w->f = NULL;
    w->mem = NULL;
    w->mem_len = w->mem_cap = 0;
    w->total_out = 0;
}

uint8_t *omnic_writer_mem_drain(omnic_writer *w, size_t *len) {
    uint8_t *p = w->mem;
    if (len != NULL) *len = w->mem_len;
    w->mem = NULL;
    w->mem_len = w->mem_cap = 0;
    return p; /* 调用方 free；NULL 时为空产物 */
}

omnic_status omnic_writer_write(omnic_writer *w, const void *buf, size_t n) {
    if (n > 0) {
        if (w->f != NULL) {
            if (fwrite(buf, 1, n, w->f) != n) return OMNIC_E_SYSTEM;
        } else {
            if (w->mem_len + n > w->mem_cap) {
                size_t cap = (w->mem_len + n) * 2;
                uint8_t *p = (uint8_t *)realloc(w->mem, cap);
                if (p == NULL) return OMNIC_E_SYSTEM;
                w->mem = p;
                w->mem_cap = cap;
            }
            memcpy(w->mem + w->mem_len, buf, n);
            w->mem_len += n;
        }
    }
    w->total_out += n;
    return OMNIC_OK;
}

uint64_t omnic_writer_total(const omnic_writer *w) { return w->total_out; }
