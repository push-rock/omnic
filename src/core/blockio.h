/* 流式块 I/O（SW-CODEC-004）：reader 支持采样后 pushback，
 * 使"探测/决策已读的字节"对 codec 仍然可见（架构 2.5）。 */
#ifndef OMNIC_BLOCKIO_H
#define OMNIC_BLOCKIO_H

#include <stdio.h>

#include "omnic.h"

typedef struct omnic_reader {
    FILE *f;
    uint8_t *pend;      /* pushback 队列（前置数据） */
    size_t pend_len;
    size_t pend_cap;
    uint64_t total_in;
} omnic_reader;

typedef struct omnic_writer {
    FILE *f;
    uint64_t total_out;
} omnic_writer;

void omnic_reader_init(omnic_reader *r, FILE *f);
void omnic_reader_fini(omnic_reader *r);
size_t omnic_reader_read(omnic_reader *r, void *buf, size_t want);
omnic_status omnic_reader_unread(omnic_reader *r, const void *buf, size_t n);
uint64_t omnic_reader_total(const omnic_reader *r);

void omnic_writer_init(omnic_writer *w, FILE *f);
omnic_status omnic_writer_write(omnic_writer *w, const void *buf, size_t n);
uint64_t omnic_writer_total(const omnic_writer *w);

#endif /* OMNIC_BLOCKIO_H */
