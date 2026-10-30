/* zlib adapter：deflate 家族三格式的流式编解码，按 windowBits 区分——
 * gzip = 15+16（RFC 1952，多 member 拼接互解）；zlib 流 = 15（RFC 1950）；
 * raw deflate = -15（无包装，识别走后缀兜底）。 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "codec/codec.h"

#define ZL_CHUNK (4u * 1024u * 1024u)

static omnic_status z_wrap_compress(omnic_reader *in, omnic_writer *out, int level, int threads, int wbits) {
    (void)threads; /* deflate 无多线程 */
    omnic_status ret = OMNIC_OK;
    z_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(ZL_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZL_CHUNK);

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL ||
        deflateInit2(&zs, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(inbuf);
        free(outbuf);
        return OMNIC_E_SYSTEM;
    }
    for (;;) {
        size_t n = omnic_reader_read(in, inbuf, ZL_CHUNK);
        zs.next_in = inbuf;
        zs.avail_in = (uInt)n;
        int flush = (n == 0) ? Z_FINISH : Z_NO_FLUSH;
        int done = 0;
        do {
            zs.next_out = outbuf;
            zs.avail_out = ZL_CHUNK;
            int r = deflate(&zs, flush);
            if (r == Z_STREAM_ERROR) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, outbuf, ZL_CHUNK - zs.avail_out) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            if (r == Z_STREAM_END) {
                done = 1;
                break;
            }
        } while (zs.avail_out == 0);
        if (done) break;
    }
done:
    deflateEnd(&zs);
    free(inbuf);
    free(outbuf);
    return ret;
}

/* STREAM_END 后 reset 续读：gzip 多 member 的标准行为；zlib/raw 单 member
 * 下若还有垃圾尾随会被当下一 member 严格校验失败，语义一致。 */
static omnic_status z_wrap_decompress(omnic_reader *in, omnic_writer *out, int wbits) {
    omnic_status ret = OMNIC_OK;
    z_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(ZL_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZL_CHUNK);
    bool got_any = false;
    int last_r = Z_OK;

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL || inflateInit2(&zs, wbits) != Z_OK) {
        free(inbuf);
        free(outbuf);
        return OMNIC_E_SYSTEM;
    }
    for (;;) {
        if (zs.avail_in == 0) {
            size_t n = omnic_reader_read(in, inbuf, ZL_CHUNK);
            if (n == 0) break;
            got_any = true;
            zs.next_in = inbuf;
            zs.avail_in = (uInt)n;
        }
        zs.next_out = outbuf;
        zs.avail_out = ZL_CHUNK;
        int r = inflate(&zs, Z_NO_FLUSH);
        if (omnic_writer_write(out, outbuf, ZL_CHUNK - zs.avail_out) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        if (r == Z_STREAM_END) {
            if (inflateReset(&zs) != Z_OK) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            last_r = Z_STREAM_END;
            continue;
        }
        if (r != Z_OK) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        last_r = r;
    }
    if (!got_any || last_r != Z_STREAM_END) ret = OMNIC_E_DATA; /* 空输入或截断 */
done:
    inflateEnd(&zs);
    free(inbuf);
    free(outbuf);
    return ret;
}

static omnic_status omnic_zlib_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    return z_wrap_compress(in, out, level, threads, 15 + 16);
}
static omnic_status omnic_zlib_decompress(omnic_reader *in, omnic_writer *out) {
    return z_wrap_decompress(in, out, 15 + 16);
}

const omnic_codec_ops omnic_codec_zlib = {
    "zlib",
    OMNIC_FMT_GZIP,
    "gz",
    true,
    true,
    0,
    9,
    6,
    omnic_zlib_compress,
    omnic_zlib_decompress,
};

static omnic_status omnic_zlibstream_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    return z_wrap_compress(in, out, level, threads, 15);
}
static omnic_status omnic_zlibstream_decompress(omnic_reader *in, omnic_writer *out) {
    return z_wrap_decompress(in, out, 15);
}

const omnic_codec_ops omnic_codec_zlibstream = {
    "zlib-stream",
    OMNIC_FMT_ZLIB,
    "zlib",
    true,
    true,
    0,
    9,
    6,
    omnic_zlibstream_compress,
    omnic_zlibstream_decompress,
};

static omnic_status omnic_rawdeflate_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    return z_wrap_compress(in, out, level, threads, -15);
}
static omnic_status omnic_rawdeflate_decompress(omnic_reader *in, omnic_writer *out) {
    return z_wrap_decompress(in, out, -15);
}

const omnic_codec_ops omnic_rawdeflate = {
    "raw-deflate",
    OMNIC_FMT_RAW_DEFLATE,
    "zz",
    true,
    true,
    0,
    9,
    6,
    omnic_rawdeflate_compress,
    omnic_rawdeflate_decompress,
};
