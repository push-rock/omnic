/* zstd adapter（SW-CODEC-001）：frame 流式编解码。
 * 多线程走 zstd 原生 nbWorkers（SYS-PERF-002）；未编译 MT 的库自动降级单线程。 */
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "codec/codec.h"

#define ZC_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_zstd_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    omnic_status ret = OMNIC_OK;
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    uint8_t *inbuf = NULL;
    uint8_t *outbuf = NULL;
    size_t outcap = ZSTD_compressBound(ZC_CHUNK);

    if (cctx == NULL) return OMNIC_E_SYSTEM;
    inbuf = (uint8_t *)malloc(ZC_CHUNK);
    outbuf = (uint8_t *)malloc(outcap);
    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    if (ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_compressionLevel, level)) ||
        ZSTD_isError(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1))) {
        ret = OMNIC_E_DATA;
        goto done;
    }
    if (threads > 1) {
        (void)ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, (unsigned)threads);
    }

    {
        ZSTD_inBuffer ib = {inbuf, 0, 0};
        /* 输入阶段：e_continue 直到 reader 枯竭。MT 模式下 e_continue 可能不满额
         * 消费 ib（作业池饱和），未耗尽时不再 refill，交给下一次调用继续消化。 */
        for (;;) {
            if (ib.pos == ib.size) {
                size_t n = omnic_reader_read(in, inbuf, ZC_CHUNK);
                ib.size = n;
                ib.pos = 0;
                if (n == 0) break;
            }
            ZSTD_outBuffer ob = {outbuf, outcap, 0};
            size_t r = ZSTD_compressStream2(cctx, &ob, &ib, ZSTD_e_continue);
            if (ZSTD_isError(r)) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, outbuf, ob.pos) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
        }
        /* 收尾阶段：e_end 循环到 r==0。MT 作业池会把全部输出攒到此刻，
         * 一个 ob 装不下——只调一次 e_end 会静默截断（v0.3.1 的 pack -T 数据损坏根因）。 */
        for (;;) {
            ZSTD_outBuffer ob = {outbuf, outcap, 0};
            size_t r = ZSTD_compressStream2(cctx, &ob, &ib, ZSTD_e_end);
            if (ZSTD_isError(r)) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, outbuf, ob.pos) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            if (r == 0) break;
        }
    }
done:
    free(inbuf);
    free(outbuf);
    ZSTD_freeCCtx(cctx);
    return ret;
}

static omnic_status omnic_zstd_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    uint8_t *inbuf = (uint8_t *)malloc(ZC_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZC_CHUNK);
    bool got_any = false;
    size_t last_r = 1;

    if (dctx == NULL || inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    {
        ZSTD_inBuffer ib = {inbuf, 0, 0};
        for (;;) {
            if (ib.pos == ib.size) {
                size_t n = omnic_reader_read(in, inbuf, ZC_CHUNK);
                if (n == 0) break;
                got_any = true;
                ib.size = n;
                ib.pos = 0;
            }
            ZSTD_outBuffer ob = {outbuf, ZC_CHUNK, 0};
            size_t r = ZSTD_decompressStream(dctx, &ob, &ib);
            if (ZSTD_isError(r)) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, outbuf, ob.pos) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            last_r = r;
            if (ib.pos < ib.size) continue; /* 同一 buffer 内可能还有下一帧 */
        }
    }
    if (!got_any || last_r != 0) ret = OMNIC_E_DATA; /* 空输入或截断 */
done:
    free(inbuf);
    free(outbuf);
    ZSTD_freeDCtx(dctx);
    return ret;
}

const omnic_codec_ops omnic_codec_zstd = {
    "zstd",
    OMNIC_FMT_ZSTD,
    "zst",
    true,
    true,
    -5, /* 负值 = --fast 模式（决策表使用 -1..-3） */
    22,
    6,
    omnic_zstd_compress,
    omnic_zstd_decompress,
};
