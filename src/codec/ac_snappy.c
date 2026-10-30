/* snappy adapter：官方 framing 容器（framing_format.txt）+ snappy 块压缩。
 * 原始 snappy 块无自描述长度也无 magic；framing 层补齐两者（流标识
 * 0xFF 06 00 00 "sNaPpY"）、CRC32C 校验与 64KB 分块，与 snzip 等工具互解。
 * 级别 1-2 为 snappy 1.3.1 原生语义（1 快 / 2 更压）。 */
#include <stdlib.h>
#include <string.h>
#include <snappy-c.h>

#include "codec/codec.h"

#define SN_BLOCK 65536u                    /* 规范的 chunk 原始数据上限 */
#define SN_MAGIC "\xff\x06\x00\x00sNaPpY"  /* 10 字节流标识 */
#define SN_CAP (2u * SN_BLOCK + 8u)        /* chunk 载荷缓冲（覆盖压缩膨胀上限） */

/* ---- CRC-32C（Castagnoli）：SSE4.2 / ARM64 硬件指令优先（~10GB/s，逐字节
 * 表驱动仅 ~500MB/s，是 framing 路径的吞吐瓶颈）；无指令集时回退表驱动，
 * 函数内惰性初始化 ---- */

#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__SSE4_2__) || defined(_MSC_VER))
#include <nmmintrin.h>
static uint32_t sn_crc32c(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n >= 8) {
        c = (uint32_t)_mm_crc32_u64(c, *(const uint64_t *)p);
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        c = (uint32_t)_mm_crc32_u32(c, *(const uint32_t *)p);
        p += 4;
        n -= 4;
    }
    while (n--) c = _mm_crc32_u8(c, *p++);
    return c ^ 0xFFFFFFFFu;
}
#elif defined(__aarch64__) && defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
static uint32_t sn_crc32c(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n >= 8) {
        c = __crc32cd(c, *(const uint64_t *)p);
        p += 8;
        n -= 8;
    }
    if (n >= 4) {
        c = __crc32cw(c, *(const uint32_t *)p);
        p += 4;
        n -= 4;
    }
    while (n--) c = __crc32cb(c, *p++);
    return c ^ 0xFFFFFFFFu;
}
#else
static uint32_t sn_crc32c(const uint8_t *p, size_t n) {
    static uint32_t tbl[256];
    static int ready = 0;
    if (!ready) {
        for (unsigned i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0x82F63B78u & (0u - (c & 1u)));
            tbl[i] = c;
        }
        ready = 1;
    }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = tbl[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
#endif

static uint32_t sn_mask(uint32_t c) { return ((c >> 15) | (c << 17)) + 0xa282ead8u; }

static bool sn_get_varint32(const uint8_t *p, size_t avail, size_t *consumed, uint32_t *val) {
    uint32_t v = 0;
    size_t k = 0;
    for (;;) {
        if (k >= avail || k >= 5) return false;
        uint8_t b = p[k++];
        v |= (uint32_t)(b & 0x7Fu) << (7u * (unsigned)(k - 1));
        if (!(b & 0x80u)) break;
    }
    *consumed = k;
    *val = v;
    return true;
}

static void sn_put_le24(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
}

static uint32_t sn_get_le24(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static uint32_t sn_get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* 批量读写：4MB 预读直切 64KB 块（省逐块中转拷贝与 fread 调用），
 * chunk 头与压缩产物在批缓冲内连续拼接、攒满一次写出（高速 codec 下
 * 逐 chunk 的 fwrite 调用链占压缩耗时 ~20%）。 */
#define SN_BATCH (4u * 1024u * 1024u)

static omnic_status omnic_snappy_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads;
    if (level < SNAPPY_MIN_COMPRESSION_LEVEL) level = SNAPPY_MIN_COMPRESSION_LEVEL;
    if (level > SNAPPY_MAX_COMPRESSION_LEVEL) level = SNAPPY_MAX_COMPRESSION_LEVEL;
    omnic_status ret = OMNIC_OK;
    uint8_t *inbuf = (uint8_t *)malloc(SN_BATCH);
    size_t outcap = (size_t)SN_BATCH + SN_BATCH / 8 + 4096; /* 覆盖最坏膨胀 + 64 个 chunk 头 */
    uint8_t *outbuf = (uint8_t *)malloc(outcap);

    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    memcpy(outbuf, SN_MAGIC, 10);
    size_t opos = 10;
    for (;;) {
        size_t n = omnic_reader_read(in, inbuf, SN_BATCH);
        if (n == 0) break; /* EOF：流以最后一个数据 chunk 收尾，无终止符 */
        for (size_t off = 0; off < n; off += SN_BLOCK) {
            size_t chunk = n - off > SN_BLOCK ? SN_BLOCK : n - off;
            uint32_t crc = sn_mask(sn_crc32c(inbuf + off, chunk));
            size_t room = outcap - opos - 8;
            size_t clen = room < chunk * 2 + 64 ? room : chunk * 2 + 64;
            snappy_status st = snappy_compress_with_level((const char *)(inbuf + off), chunk, level,
                                                          (char *)(outbuf + opos + 8), &clen);
            if (st != SNAPPY_OK) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            /* compressed chunk 的 payload 即完整 snappy block（自带 varint 前缀）；
             * 无收益时退化为 uncompressed chunk（两种 chunk 的头部成本同为 8B） */
            uint8_t type = 0x00;
            size_t plen = clen;
            if (clen >= chunk) {
                type = 0x01;
                plen = chunk;
                memcpy(outbuf + opos + 8, inbuf + off, chunk);
            }
            outbuf[opos] = type;
            sn_put_le24(outbuf + opos + 1, (uint32_t)(plen + 4));
            outbuf[opos + 4] = (uint8_t)(crc >> 24);
            outbuf[opos + 5] = (uint8_t)(crc >> 16);
            outbuf[opos + 6] = (uint8_t)(crc >> 8);
            outbuf[opos + 7] = (uint8_t)crc;
            opos += 8 + plen;
        }
        if (n < SN_BATCH) break; /* 短读即 EOF */
        if (omnic_writer_write(out, outbuf, opos) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        opos = 0;
    }
    if (opos > 0 && omnic_writer_write(out, outbuf, opos) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
done:
    free(inbuf);
    free(outbuf);
    return ret;
}

static omnic_status omnic_snappy_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    uint8_t *cbuf = (uint8_t *)malloc(SN_CAP);
    uint8_t *raw = (uint8_t *)malloc(SN_BLOCK + 16);
    uint8_t hdr[10];

    if (cbuf == NULL || raw == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    if (omnic_reader_read(in, hdr, 10) != 10 || memcmp(hdr, SN_MAGIC, 10) != 0) {
        ret = OMNIC_E_DATA; /* 非法流标识 */
        goto done;
    }
    for (;;) {
        uint8_t ch[4];
        size_t h = omnic_reader_read(in, ch, 4);
        if (h == 0) break;    /* 正常 EOF */
        if (h != 4) {
            ret = OMNIC_E_DATA; /* 截断的 chunk 头 */
            goto done;
        }
        uint8_t type = ch[0];
        uint32_t total = sn_get_le24(ch + 1); /* CRC(4) + payload */
        if (total < 4 || total > SN_CAP) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        if (omnic_reader_read(in, cbuf, total) != total) {
            ret = OMNIC_E_DATA; /* 截断的 chunk 体 */
            goto done;
        }
        uint32_t stored = sn_get_be32(cbuf);
        const uint8_t *payload = cbuf + 4;
        size_t plen = total - 4;
        if (type == 0x00) { /* compressed：payload 为完整 snappy block（自带 varint 前缀） */
            uint32_t ulen32;
            size_t vlen;
            if (!sn_get_varint32(payload, plen, &vlen, &ulen32) || ulen32 > SN_BLOCK) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            size_t uclen = ulen32;
            snappy_status st = snappy_uncompress((const char *)payload, plen, (char *)raw, &uclen);
            if (st != SNAPPY_OK || uclen != ulen32) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (sn_mask(sn_crc32c(raw, uclen)) != stored) {
                ret = OMNIC_E_DATA; /* CRC 不符 */
                goto done;
            }
            if (omnic_writer_write(out, raw, uclen) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
        } else if (type == 0x01) { /* uncompressed */
            if (plen > SN_BLOCK) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (sn_mask(sn_crc32c(payload, plen)) != stored) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, payload, plen) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
        } else if (type == 0xFE) {
            /* padding：忽略 */
        } else if (type == 0xFF) {
            /* 中途出现的流标识：校验后忽略 */
            if (plen != 6 || memcmp(payload, "sNaPpY", 6) != 0) {
                ret = OMNIC_E_DATA;
                goto done;
            }
        } else {
            ret = OMNIC_E_DATA; /* 保留类型 */
            goto done;
        }
    }
done:
    free(cbuf);
    free(raw);
    return ret;
}

const omnic_codec_ops omnic_codec_snappy = {
    "snappy",
    OMNIC_FMT_SNAPPY,
    "snappy",
    true,
    true,
    1,
    2,
    1,
    omnic_snappy_compress,
    omnic_snappy_decompress,
};
