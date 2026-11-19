/* zstd adapter（SW-CODEC-001）：frame 流式编解码。
 * 多线程走 zstd 原生 nbWorkers（SYS-PERF-002）；未编译 MT 的库自动降级单线程。
 * ctx/缓冲线程本地复用（T1/A1）：归档路径每块一调，per-file 是数千次调用，
 * 每次从零建 DCtx/CCtx + 2×4MiB malloc 是纯固定税。复用是官方支持路径
 * （参数每次重设，无跨流残留）；进程退出不释放——省去 TLS 析构的顺序问题。 */
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "codec/codec.h"

#define ZC_CHUNK (4u * 1024u * 1024u)

typedef struct {
    ZSTD_CCtx *cctx;
    ZSTD_DCtx *dctx;
    uint8_t *inbuf;
    uint8_t *outbuf;
    size_t outcap;
    int ok; /* 0=未分配（首调或 OOM 退回后） */
} zc_tls;

static _Thread_local zc_tls zc;

/* 取 TLS 缓存；任何一步失败置 ok=0，调用方退回一次性分配（正确性优先） */
static int zc_buf(void) {
    if (zc.ok) return 0;
    if (zc.inbuf == NULL) zc.inbuf = (uint8_t *)malloc(ZC_CHUNK);
    if (zc.outbuf == NULL) {
        zc.outcap = ZSTD_compressBound(ZC_CHUNK);
        zc.outbuf = (uint8_t *)malloc(zc.outcap);
    }
    if (zc.inbuf == NULL || zc.outbuf == NULL) return -1;
    zc.ok = 1;
    return 0;
}

static omnic_status omnic_zstd_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    omnic_status ret = OMNIC_OK;
    int own = 0; /* 1=本次调用自建对象（TLS 不可用），用毕释放 */
    ZSTD_CCtx *cctx = zc.cctx;
    uint8_t *inbuf = NULL;
    uint8_t *outbuf = NULL;
    size_t outcap = ZSTD_compressBound(ZC_CHUNK);

    if (cctx == NULL) {
        cctx = ZSTD_createCCtx();
        if (cctx == NULL) return OMNIC_E_SYSTEM;
        if (zc_buf() == 0) {
            zc.cctx = cctx; /* TLS 建成：缓冲并入复用池 */
            inbuf = zc.inbuf;
            outbuf = zc.outbuf;
            outcap = zc.outcap;
        } else {
            own = 1; /* 缓冲 OOM：一次性分配，ctx 也随用随放 */
            inbuf = (uint8_t *)malloc(ZC_CHUNK);
            outbuf = (uint8_t *)malloc(outcap);
            if (inbuf == NULL || outbuf == NULL) {
                free(inbuf);
                free(outbuf);
                ZSTD_freeCCtx(cctx);
                return OMNIC_E_SYSTEM;
            }
        }
    } else if (zc_buf() != 0) {
        own = 1; /* 缓冲中途失效（理论不可达）：保守退回 */
        inbuf = (uint8_t *)malloc(ZC_CHUNK);
        outbuf = (uint8_t *)malloc(outcap);
        if (inbuf == NULL || outbuf == NULL) {
            free(inbuf);
            free(outbuf);
            return OMNIC_E_SYSTEM;
        }
    } else {
        inbuf = zc.inbuf;
        outbuf = zc.outbuf;
        outcap = zc.outcap;
    }

    /* 复用 CCtx 前复位会话（参数重设在下方 setParameter）：上一流失败退出时
     * 的半开帧状态不能带进本流。 */
    if (!own && ZSTD_isError(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_only))) {
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
    if (own) {
        free(inbuf);
        free(outbuf);
        ZSTD_freeCCtx(cctx);
    }
    return ret;
}

static omnic_status omnic_zstd_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    int own = 0;
    ZSTD_DCtx *dctx = zc.dctx;
    uint8_t *inbuf = NULL;
    uint8_t *outbuf = NULL;

    if (dctx == NULL) {
        dctx = ZSTD_createDCtx();
        if (dctx == NULL) return OMNIC_E_SYSTEM;
        if (zc_buf() == 0) {
            zc.dctx = dctx;
            inbuf = zc.inbuf;
            outbuf = zc.outbuf;
        } else {
            own = 1;
            inbuf = (uint8_t *)malloc(ZC_CHUNK);
            outbuf = (uint8_t *)malloc(ZC_CHUNK);
            if (inbuf == NULL || outbuf == NULL) {
                free(inbuf);
                free(outbuf);
                ZSTD_freeDCtx(dctx);
                return OMNIC_E_SYSTEM;
            }
        }
    } else if (zc_buf() != 0) {
        own = 1;
        inbuf = (uint8_t *)malloc(ZC_CHUNK);
        outbuf = (uint8_t *)malloc(ZC_CHUNK);
        if (inbuf == NULL || outbuf == NULL) {
            free(inbuf);
            free(outbuf);
            return OMNIC_E_SYSTEM;
        }
    } else {
        inbuf = zc.inbuf;
        outbuf = zc.outbuf;
    }
    /* 复用 DCtx 前复位：上一流若未消费完（坏流中途退出），残余状态会污染
     * 下一流的解压（ZSTD_decompressStream 返回 data error）。官方复用路径。 */
    if (!own && ZSTD_isError(ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only))) {
        return OMNIC_E_SYSTEM;
    }
    bool got_any = false;
    size_t last_r = 1;

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
    if (own) {
        free(inbuf);
        free(outbuf);
        ZSTD_freeDCtx(dctx);
    }
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
