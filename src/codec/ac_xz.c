/* xz adapter（SW-CODEC-001）：liblzma stream API，CRC64 校验（SYS-DEC-005）。
 * 另含 lzma-alone（.lzma）条目：同一 liblzma 的 alone 编解码器，13 字节头、
 * 无容器校验，流循环与 xz 完全共用。 */
#include <stdint.h>
#include <stdlib.h>

#include "codec/codec.h"
#include <lzma.h>

#define XZ_CHUNK (4u * 1024u * 1024u)

/* strm 已初始化：驱动压缩循环并负责收尾。 */
static omnic_status lz_run_encode(omnic_reader *in, omnic_writer *out, lzma_stream *strm) {
    omnic_status ret = OMNIC_OK;
    uint8_t *inbuf = (uint8_t *)malloc(XZ_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(XZ_CHUNK);
    lzma_action action = LZMA_RUN;

    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    for (;;) {
        if (strm->avail_in == 0 && action == LZMA_RUN) {
            size_t n = omnic_reader_read(in, inbuf, XZ_CHUNK);
            if (n == 0) {
                action = LZMA_FINISH;
            } else {
                strm->next_in = inbuf;
                strm->avail_in = n;
            }
        }
        strm->next_out = outbuf;
        strm->avail_out = XZ_CHUNK;
        lzma_ret r = lzma_code(strm, action);
        size_t produced = XZ_CHUNK - strm->avail_out;
        if (produced > 0 && omnic_writer_write(out, outbuf, produced) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            break;
        }
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) {
            ret = OMNIC_E_DATA;
            break;
        }
    }
    lzma_end(strm);
done:
    free(inbuf);
    free(outbuf);
    return ret;
}

/* strm 已初始化：驱动解压循环并负责收尾。输入耗尽后改送 LZMA_FINISH——
 * .lz 多 member 解码器靠它判定"没有下一个 member"并正常收尾（仅 LZMA_RUN
 * 会永远等待下一 member 头）。EOF 后仍未达 STREAM_END 判截断。 */
static omnic_status lz_run_decode(omnic_reader *in, omnic_writer *out, lzma_stream *strm) {
    omnic_status ret = OMNIC_OK;
    uint8_t *inbuf = (uint8_t *)malloc(XZ_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(XZ_CHUNK);
    bool got_any = false;
    bool reached_end = false;
    lzma_action action = LZMA_RUN;

    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    for (;;) {
        if (strm->avail_in == 0 && action == LZMA_RUN) {
            size_t n = omnic_reader_read(in, inbuf, XZ_CHUNK);
            if (n == 0) {
                action = LZMA_FINISH;
            } else {
                got_any = true;
                strm->next_in = inbuf;
                strm->avail_in = n;
            }
        }
        strm->next_out = outbuf;
        strm->avail_out = XZ_CHUNK;
        lzma_ret r = lzma_code(strm, action);
        size_t produced = XZ_CHUNK - strm->avail_out;
        if (produced > 0 && omnic_writer_write(out, outbuf, produced) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            break;
        }
        if (r == LZMA_STREAM_END) {
            reached_end = true;
            break;
        }
        if (r != LZMA_OK) { /* DATA_ERROR / BUF_ERROR（FINISH 下无进展=截断）等 */
            ret = OMNIC_E_DATA;
            break;
        }
    }
    if (!got_any || !reached_end) ret = OMNIC_E_DATA; /* 空输入或截断 */
    lzma_end(strm);
done:
    free(inbuf);
    free(outbuf);
    return ret;
}

static omnic_status omnic_xz_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads; /* 一期单线程（SYS-PERF-002） */
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_easy_encoder(&strm, (uint32_t)level, LZMA_CHECK_CRC64) != LZMA_OK) return OMNIC_E_SYSTEM;
    return lz_run_encode(in, out, &strm);
}

static omnic_status omnic_xz_decompress(omnic_reader *in, omnic_writer *out) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK) return OMNIC_E_SYSTEM;
    return lz_run_decode(in, out, &strm);
}

const omnic_codec_ops omnic_codec_xz = {
    "xz",
    OMNIC_FMT_XZ,
    "xz",
    true,
    true,
    0,
    9,
    6,
    omnic_xz_compress,
    omnic_xz_decompress,
};

static omnic_status omnic_lzma_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads;
    lzma_stream strm = LZMA_STREAM_INIT;
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, (uint32_t)level) != LZMA_OK) return OMNIC_E_SYSTEM;
    if (lzma_alone_encoder(&strm, &opt) != LZMA_OK) return OMNIC_E_SYSTEM;
    return lz_run_encode(in, out, &strm);
}

static omnic_status omnic_lzma_decompress(omnic_reader *in, omnic_writer *out) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_alone_decoder(&strm, UINT64_MAX) != LZMA_OK) return OMNIC_E_SYSTEM;
    return lz_run_decode(in, out, &strm);
}

const omnic_codec_ops omnic_codec_lzma = {
    "lzma",
    OMNIC_FMT_LZMA_ALONE,
    "lzma",
    true,
    true,
    0,
    9,
    6,
    omnic_lzma_compress,
    omnic_lzma_decompress,
};

/* lzip 条目（需求 4.4 节）：.lz 容器（头 + raw LZMA1 + 20 字节 CRC32/尺寸尾）
 * 的解析与校验全部由 liblzma 的 lzip decoder 完成。CONCATENATED 语义与
 * xz -dc 对齐：多 member 全解，非 "L" 开头的尾随数据忽略。上游无 lzip
 * 编码器，仅解码。 */
static omnic_status omnic_lzip_decompress(omnic_reader *in, omnic_writer *out) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (lzma_lzip_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) return OMNIC_E_SYSTEM;
    return lz_run_decode(in, out, &strm);
}

const omnic_codec_ops omnic_codec_lzip = {
    "lzip",
    OMNIC_FMT_LZIP,
    "lz",
    false,
    true,
    0,
    0,
    0,
    NULL,
    omnic_lzip_decompress,
};
