#include "core/blockio.h"

#include <stdlib.h>
#include <string.h>

void omnic_reader_init(omnic_reader *r, FILE *f) {
    r->f = f;
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
        got += fread((char *)buf + got, 1, want - got, r->f);
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
    w->total_out = 0;
}

omnic_status omnic_writer_write(omnic_writer *w, const void *buf, size_t n) {
    if (n > 0 && fwrite(buf, 1, n, w->f) != n) return OMNIC_E_SYSTEM;
    w->total_out += n;
    return OMNIC_OK;
}

uint64_t omnic_writer_total(const omnic_writer *w) { return w->total_out; }
