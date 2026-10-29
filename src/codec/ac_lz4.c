/* lz4 adapter（SW-CODEC-001）：LZ4 frame 格式（.lz4）。
 * level 0 = fast 模式（决策表快压档）；1..12 = LZ4HC。 */
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include <lz4frame.h>

#define LZ_CHUNK (4u * 1024u * 1024u)

static omnic_status omnic_lz4_compress(omnic_reader *in, omnic_writer *out, int level, int threads) {
    omnic_status ret = OMNIC_OK;
    LZ4F_cctx *cctx = NULL;
    uint8_t *inbuf = NULL;
    uint8_t *outbuf = NULL;
    LZ4F_preferences_t pref;
    size_t outcap;

    (void)threads; /* lz4 frame API 无并行 */

    memset(&pref, 0, sizeof(pref));
    pref.compressionLevel = level;
    pref.frameInfo.contentChecksumFlag = 1; /* SW-CODEC：透传格式内建校验 */
    pref.frameInfo.blockSizeID = LZ4F_max256KB;

    if (LZ4F_isError(LZ4F_createCompressionContext(&cctx, LZ4F_VERSION))) return OMNIC_E_SYSTEM;
    outcap = LZ4F_compressBound(LZ_CHUNK, &pref);
    inbuf = (uint8_t *)malloc(LZ_CHUNK);
    outbuf = (uint8_t *)malloc(outcap);
    if (inbuf == NULL || outbuf == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    {
        size_t n = LZ4F_compressBegin(cctx, outbuf, outcap, &pref);
        if (LZ4F_isError(n)) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        if (omnic_writer_write(out, outbuf, n) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
    }
    for (;;) {
        size_t got = omnic_reader_read(in, inbuf, LZ_CHUNK);
        if (got == 0) break;
        size_t n = LZ4F_compressUpdate(cctx, outbuf, outcap, inbuf, got, NULL);
        if (LZ4F_isError(n)) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        if (omnic_writer_write(out, outbuf, n) != OMNIC_OK) {
            ret = OMNIC_E_SYSTEM;
            goto done;
        }
    }
    {
        size_t n = LZ4F_compressEnd(cctx, outbuf, outcap, NULL);
        if (LZ4F_isError(n)) {
            ret = OMNIC_E_DATA;
            goto done;
        }
        if (omnic_writer_write(out, outbuf, n) != OMNIC_OK) ret = OMNIC_E_SYSTEM;
    }
done:
    free(inbuf);
    free(outbuf);
    if (cctx != NULL) LZ4F_freeCompressionContext(cctx);
    return ret;
}

static omnic_status omnic_lz4_decompress(omnic_reader *in, omnic_writer *out) {
    omnic_status ret = OMNIC_OK;
    LZ4F_dctx *dctx = NULL;
    uint8_t *inbuf = (uint8_t *)malloc(LZ_CHUNK);
    uint8_t *outbuf = (uint8_t *)malloc(LZ_CHUNK);
    bool got_any = false;
    size_t last_r = 1;

    if (inbuf == NULL || outbuf == NULL || LZ4F_isError(LZ4F_createDecompressionContext(&dctx, LZ4F_VERSION))) {
        ret = OMNIC_E_SYSTEM;
        goto done;
    }
    {
        size_t have = 0, pos = 0;
        for (;;) {
            if (pos == have) {
                size_t n = omnic_reader_read(in, inbuf, LZ_CHUNK);
                if (n == 0) break;
                got_any = true;
                have = n;
                pos = 0;
            }
            size_t outn = LZ_CHUNK;
            size_t inn = have - pos;
            size_t r = LZ4F_decompress(dctx, outbuf, &outn, inbuf + pos, &inn, NULL);
            if (LZ4F_isError(r)) {
                ret = OMNIC_E_DATA;
                goto done;
            }
            pos += inn;
            if (omnic_writer_write(out, outbuf, outn) != OMNIC_OK) {
                ret = OMNIC_E_SYSTEM;
                goto done;
            }
            last_r = r; /* r==0 表示当前帧完整 */
        }
    }
    if (!got_any || last_r != 0) ret = OMNIC_E_DATA; /* 空输入或截断 */
done:
    free(inbuf);
    free(outbuf);
    if (dctx != NULL) LZ4F_freeDecompressionContext(dctx);
    return ret;
}

const omnic_codec_ops omnic_codec_lz4 = {
    "lz4",
    OMNIC_FMT_LZ4,
    "lz4",
    true,
    true,
    0, /* 0 = fast 模式 */
    12,
    0,
    omnic_lz4_compress,
    omnic_lz4_decompress,
};
