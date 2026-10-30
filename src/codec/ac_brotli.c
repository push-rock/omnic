/* brotli adapter：.br 原生流式编解码。格式无 magic，识别走后缀兜底。
 * 级别 0-11 为原生语义；LGWIN 固定 22（4MB 块下最优窗口）。
 * 解压注意：输出填满会以 NEEDS_MORE_INPUT 暂停（而非 NEEDS_MORE_OUTPUT），
 * 输入耗尽时必须先用新输出缓冲重调，仍无进展才判定截断。 */
#include <stdlib.h>
#include <string.h>
#include <brotli/decode.h>
#include <brotli/encode.h>

#include "codec/codec.h"

#define BR_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_brotli_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads; /* 一期单线程；brotli 多线程需外层分块方案 */
    omnic_status ret = OMNIC_OK;
    BrotliEncoderState *s = BrotliEncoderCreateInstance(NULL, NULL, NULL);
    uint8_t *inbuf = (uint8_t *)malloc(BR_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(BR_CHUNK);
    bool eof = false;

    if (s == NULL || inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    if (!BrotliEncoderSetParameter(s, BROTLI_PARAM_QUALITY, (uint32_t)level) ||
        !BrotliEncoderSetParameter(s, BROTLI_PARAM_LGWIN, 22)) {
        ret = OMNIC_E_DATA;
        goto done;
    }
    for (;;) {
        size_t avail_in = 0;
        const uint8_t *next_in = inbuf;
        if (!eof) {
            size_t n = omnic_reader_read(in, inbuf, BR_CHUNK);
            avail_in = n;
            eof = (n == 0);
        }
        BrotliEncoderOperation op = eof ? BROTLI_OPERATION_FINISH : BROTLI_OPERATION_PROCESS;
        do {
            size_t avail_out = BR_CHUNK;
            uint8_t *next_out = outbuf;
            if (!BrotliEncoderCompressStream(s, op, &avail_in, &next_in, &avail_out, &next_out, NULL)) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, outbuf, BR_CHUNK - avail_out) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
        } while (avail_in > 0 || BrotliEncoderHasMoreOutput(s));
        if (eof && BrotliEncoderIsFinished(s)) break;
    }
done:
    free(inbuf);
    free(outbuf);
    if (s != NULL) BrotliEncoderDestroyInstance(s);
    return ret;
}

static omnic_status omnic_brotli_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    BrotliDecoderState *s = BrotliDecoderCreateInstance(NULL, NULL, NULL);
    uint8_t *inbuf = (uint8_t *)malloc(BR_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(BR_CHUNK);
    bool done = false;
    bool input_eof = false;
    size_t avail_in = 0;
    const uint8_t *next_in = inbuf;

    if (s == NULL || inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    for (;;) {
        if (avail_in == 0 && !input_eof) {
            size_t n = omnic_reader_read(in, inbuf, BR_CHUNK);
            if (n == 0) {
                input_eof = true;
            } else {
                avail_in = n;
                next_in = inbuf;
            }
        }
        size_t avail_out = BR_CHUNK;
        uint8_t *next_out = outbuf;
        BrotliDecoderResult r = BrotliDecoderDecompressStream(s, &avail_in, &next_in, &avail_out, &next_out, NULL);
        if (omnic_writer_write(out, outbuf, BR_CHUNK - avail_out) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        if (r == BROTLI_DECODER_RESULT_SUCCESS) {
            done = true;
            break;
        }
        if (r == BROTLI_DECODER_RESULT_ERROR) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        /* 输入耗尽且无输入可读且本次有剩余输出空间仍报缺输入：真截断。
         * 输出恰好填满时的暂停也报 NEEDS_MORE_INPUT——下一轮腾出输出
         * 缓冲重调即可继续，不能在这里判定失败。 */
        if (r == BROTLI_DECODER_RESULT_NEEDS_MORE_INPUT && input_eof && avail_in == 0 && avail_out > 0) break;
    }
    if (!done) ret = OMNIC_E_DATA; /* EOF 前未完成 = 截断 */
done:
    free(inbuf);
    free(outbuf);
    if (s != NULL) BrotliDecoderDestroyInstance(s);
    return ret;
}

const omnic_codec_ops omnic_codec_brotli = {
    "brotli",
    OMNIC_FMT_BROTLI,
    "br",
    true,
    true,
    0,
    11,
    6,
    omnic_brotli_compress,
    omnic_brotli_decompress,
};
