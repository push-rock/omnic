/* xz adapter（SW-CODEC-001）：liblzma stream API，CRC64 校验（SYS-DEC-005）。
 * 另含 lzma-alone（.lzma）条目：同一 liblzma 的 alone 编解码器，13 字节头、
 * 无容器校验，流循环与 xz 完全共用。 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* MT 内存预算（liblzma 文档：每线程约 3×block_size）。-9 满线程可到 10+ GiB，
 * 超预算自动降线程，地板 2——宁可慢也要能跑。 */
#define XZ_MT_MEM_CAP ((uint64_t)2 << 30)

/* 小输入字典右调（A2）：匹配距离不可能超过输入长度，avail 已知且 < 8MiB 时
 * 字典取 2 的幂紧上界（LZMA 字典约束），上限 preset 默认。大输入/未知尺寸
 * 一律全尺寸——行为与旧版逐字节一致。 */
static uint32_t lz_small_dict(uint64_t avail, uint32_t preset_dict) {
    if (avail == UINT64_MAX || avail == 0 || avail >= (8u << 20)) return preset_dict;
    uint32_t d = 4096; /* LZMA 字典下限 */
    while ((uint64_t)d < avail) d <<= 1;
    return d < preset_dict ? d : preset_dict;
}

static omnic_status omnic_xz_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    lzma_stream strm = LZMA_STREAM_INIT;
    if (threads > 1) {
        lzma_mt mt;
        memset(&mt, 0, sizeof(mt));
        mt.threads = (uint32_t)threads;
        mt.block_size = 0; /* liblzma 自取 3×dict（xz CLI 同款约定） */
        mt.preset = (uint32_t)level;
        mt.check = LZMA_CHECK_CRC64;
        while (mt.threads > 2 && lzma_stream_encoder_mt_memusage(&mt) > XZ_MT_MEM_CAP) mt.threads--;
        if (lzma_stream_encoder_mt(&strm, &mt) != LZMA_OK) {
            /* MT 初始化失败（内存压力等）退回单线程路径 */
            if (lzma_easy_encoder(&strm, (uint32_t)level, LZMA_CHECK_CRC64) != LZMA_OK) {
                return OMNIC_E_SYSTEM;
            }
        }
    } else {
        /* 单线程小输入：显式 options 编码（dict 右调，A2）。avail 未知或
         * ≥8MiB 时 lz_small_dict 原样返回 preset 字典，走 easy_encoder，
         * 产物与旧版逐字节一致。 */
        uint64_t avail = omnic_reader_avail(in);
        if (avail != UINT64_MAX) {
            lzma_options_lzma opt;
            if (lzma_lzma_preset(&opt, (uint32_t)level) != LZMA_OK) return OMNIC_E_SYSTEM;
            uint32_t pd = opt.dict_size;
            opt.dict_size = lz_small_dict(avail, pd);
            if (opt.dict_size != pd) {
                lzma_filter filters[2];
                filters[0].id = LZMA_FILTER_LZMA2;
                filters[0].options = &opt;
                filters[1].id = LZMA_VLI_UNKNOWN;
                filters[1].options = NULL;
                if (lzma_stream_encoder(&strm, filters, LZMA_CHECK_CRC64) != LZMA_OK)
                    return OMNIC_E_SYSTEM;
                return lz_run_encode(in, out, &strm);
            }
        }
        if (lzma_easy_encoder(&strm, (uint32_t)level, LZMA_CHECK_CRC64) != LZMA_OK) {
            return OMNIC_E_SYSTEM;
        }
    }
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
