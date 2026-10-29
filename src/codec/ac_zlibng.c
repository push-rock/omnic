/* zlib-ng adapter：gzip 格式（RFC 1952）流式编解码，与 zlib 产出同格式流。
 * 非兼容构建（zlib-ng.h / zng_ 前缀符号），可与 zlib 静态共存于同一二进制。 */
#include <stdlib.h>
#include <string.h>
#include <zlib-ng.h>

#include "codec/codec.h"

#define ZN_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_zlibng_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads;
    omnic_status ret = OMNIC_OK;
    zng_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(ZN_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZN_CHUNK);

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL ||
        zng_deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        free(inbuf);
        free(outbuf);
        return OMNIC_E_SYSTEM;
    }
    for (;;) {
        size_t n = omnic_reader_read(in, inbuf, ZN_CHUNK);
        zs.next_in = inbuf;
        zs.avail_in = (uint32_t)n;
        int flush = (n == 0) ? Z_FINISH : Z_NO_FLUSH;
        int done = 0;
        do {
            zs.next_out = outbuf;
            zs.avail_out = ZN_CHUNK;
            int r = zng_deflate(&zs, flush);
            if (r == Z_STREAM_ERROR) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            if (omnic_writer_write(out, outbuf, ZN_CHUNK - zs.avail_out) != OMNIC_OK) {
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
    zng_deflateEnd(&zs);
    free(inbuf);
    free(outbuf);
    return ret;
}

static omnic_status omnic_zlibng_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    zng_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(ZN_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZN_CHUNK);
    bool got_any = false;
    int last_r = Z_OK;

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL || zng_inflateInit2(&zs, 15 + 16) != Z_OK) {
        free(inbuf);
        free(outbuf);
        return OMNIC_E_SYSTEM;
    }
    for (;;) {
        if (zs.avail_in == 0) {
            size_t n = omnic_reader_read(in, inbuf, ZN_CHUNK);
            if (n == 0) break;
            got_any = true;
            zs.next_in = inbuf;
            zs.avail_in = (uint32_t)n;
        }
        zs.next_out = outbuf;
        zs.avail_out = ZN_CHUNK;
        int r = zng_inflate(&zs, Z_NO_FLUSH);
        if (omnic_writer_write(out, outbuf, ZN_CHUNK - zs.avail_out) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
        if (r == Z_STREAM_END) {
            if (zng_inflateReset(&zs) != Z_OK) {
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
    if (!got_any || last_r != Z_STREAM_END) ret = OMNIC_E_DATA;
done:
    zng_inflateEnd(&zs);
    free(inbuf);
    free(outbuf);
    return ret;
}

const omnic_codec_ops omnic_codec_zlibng = {
    "zlib-ng",
    OMNIC_FMT_GZIP,
    "gz",
    true,
    true,
    0,
    9,
    6,
    omnic_zlibng_compress,
    omnic_zlibng_decompress,
};
