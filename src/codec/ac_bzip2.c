/* bzip2 adapter（需求 4.4）：vendored 解码半边（bzlib_dec.c，SW-CODEC-005），
 * 流式 API。串接流（cat a.bz2 b.bz2）在 STREAM_END 后重开解码器继续；
 * 最后一个流未达 STREAM_END 即 EOF 判截断。仅解码。 */
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>

#include "codec/codec.h"

#define BZ2C_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_bzip2_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    bz_stream zs;
    uint8_t *inbuf = (uint8_t *)malloc(BZ2C_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(BZ2C_CHUNK);
    bool inited = false;
    bool got_any = false;
    bool cur_done = false; /* 当前流已完整解码到 STREAM_END */

    memset(&zs, 0, sizeof(zs));
    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    if (BZ2_bzDecompressInit(&zs, 0, 0) != BZ_OK) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    inited = true;
    for (;;) {
        if (cur_done) {
            /* 上一流已尽：读入更多（优先用缓冲剩余），重开新流。
             * Init 不改写 next_in/avail_in，剩余输入跨流保留。 */
            if (zs.avail_in == 0) {
                size_t n = omnic_reader_read(in, inbuf, BZ2C_CHUNK);
                if (n == 0) break; /* EOF：全部完成 */
                got_any = true;
                zs.next_in = (char *)inbuf;
                zs.avail_in = (unsigned int)n;
            }
            BZ2_bzDecompressEnd(&zs);
            if (BZ2_bzDecompressInit(&zs, 0, 0) != BZ_OK) {
                inited = false;
                ret = OMNIC_E_SYSTEM;
                break;
            }
            cur_done = false;
            continue;
        }
        if (zs.avail_in == 0) {
            size_t n = omnic_reader_read(in, inbuf, BZ2C_CHUNK);
            if (n == 0) break; /* EOF：当前流未竟 = 截断 */
            got_any = true;
            zs.next_in = (char *)inbuf;
            zs.avail_in = (unsigned int)n;
        }
        zs.next_out = (char *)outbuf;
        zs.avail_out = BZ2C_CHUNK;
        int r = BZ2_bzDecompress(&zs);
        size_t produced = BZ2C_CHUNK - zs.avail_out;
        if (produced > 0 && omnic_writer_write(out, outbuf, produced) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            break;
        }
        if (r == BZ_STREAM_END) {
            cur_done = true;
            continue;
        }
        if (r != BZ_OK) {
            ret = OMNIC_E_DATA;
            break;
        }
    }
    if (!got_any || !cur_done) ret = OMNIC_E_DATA; /* 空输入或截断 */
done:
    if (inited) BZ2_bzDecompressEnd(&zs);
    free(inbuf);
    free(outbuf);
    return ret;
}

const omnic_codec_ops omnic_codec_bzip2 = {
    "bzip2",
    OMNIC_FMT_BZIP2,
    "bz2",
    false,
    true,
    0,
    0,
    0,
    NULL,
    omnic_bzip2_decompress,
};
