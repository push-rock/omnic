/* xz adapter（SW-CODEC-001）：liblzma stream API，CRC64 校验（SYS-DEC-005）。 */
#include <stdint.h>
#include <stdlib.h>

#include "codec/codec.h"
#include <lzma.h>

#define XZ_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_xz_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    omnic_status ret = OMNIC_OK;
    lzma_stream strm = LZMA_STREAM_INIT;
    uint8_t *inbuf = (uint8_t *)malloc(XZ_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(XZ_CHUNK);
    lzma_action action = LZMA_RUN;

    (void)threads; /* 一期单线程（SYS-PERF-002） */

    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    if (lzma_easy_encoder(&strm, (uint32_t)level, LZMA_CHECK_CRC64) != LZMA_OK) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    for (;;) {
        if (strm.avail_in == 0 && action == LZMA_RUN) {
            size_t n = omnic_reader_read(in, inbuf, XZ_CHUNK);
            if (n == 0) {
                action = LZMA_FINISH;
            } else {
                strm.next_in = inbuf;
                strm.avail_in = n;
            }
        }
        strm.next_out = outbuf;
        strm.avail_out = XZ_CHUNK;
        lzma_ret r = lzma_code(&strm, action);
        size_t produced = XZ_CHUNK - strm.avail_out;
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
    lzma_end(&strm);
done:
    free(inbuf);
    free(outbuf);
    return ret;
}

static omnic_status omnic_xz_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    lzma_stream strm = LZMA_STREAM_INIT;
    uint8_t *inbuf = (uint8_t *)malloc(XZ_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(XZ_CHUNK);
    bool got_any = false;
    bool reached_end = false;

    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    if (lzma_stream_decoder(&strm, UINT64_MAX, 0) != LZMA_OK) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    for (;;) {
        if (strm.avail_in == 0) {
            size_t n = omnic_reader_read(in, inbuf, XZ_CHUNK);
            if (n == 0) break; /* EOF：截断与否由 reached_end 判定 */
            got_any = true;
            strm.next_in = inbuf;
            strm.avail_in = n;
        }
        strm.next_out = outbuf;
        strm.avail_out = XZ_CHUNK;
        lzma_ret r = lzma_code(&strm, LZMA_RUN);
        size_t produced = XZ_CHUNK - strm.avail_out;
        if (produced > 0 && omnic_writer_write(out, outbuf, produced) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            break;
        }
        if (r == LZMA_STREAM_END) {
            reached_end = true; /* 流完整（格式内校验通过） */
            break;
        }
        if (r != LZMA_OK) {
            ret = OMNIC_E_DATA;
            break;
        }
    }
    if (!got_any || !reached_end) ret = OMNIC_E_DATA; /* 空输入或截断 */
    lzma_end(&strm);
done:
    free(inbuf);
    free(outbuf);
    return ret;
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
