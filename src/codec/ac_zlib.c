/* zlib adapter：gzip 格式（RFC 1952）流式编解码，产出与 gunzip 完全互解。
 * 解压支持多 member 拼接流（cat a.gz b.gz / libdeflate 分块产物）。 */
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "codec/codec.h"

#define ZL_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_zlib_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    (void)threads; /* deflate 无多线程 */
    omnic_status ret = OMNIC_OK;
    z_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(ZL_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZL_CHUNK);

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL ||
        deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
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

static omnic_status omnic_zlib_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    z_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(ZL_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(ZL_CHUNK);
    bool got_any = false;
    int last_r = Z_OK;

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL || inflateInit2(&zs, 15 + 16) != Z_OK) {
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
            /* 多 member 流：重置后继续消化剩余输入 */
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
