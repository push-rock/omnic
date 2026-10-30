/* zlib-ng 基线（bench.c 拆分）：zlib-ng.h 与 zlib.h 的符号/宏在同一个
 * 编译单元里冲突，单独编译本文件提供基线函数。 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib-ng.h>

static void zng_die(void) { exit(2); }

static size_t base_zlibng_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    zng_stream zs;
    uint8_t *ob = (uint8_t *)malloc(cap);
    if (ob == NULL) zng_die();
    memset(&zs, 0, sizeof(zs));
    if (zng_deflateInit2(&zs, lvl, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) zng_die();
    size_t produced = 0;
    for (size_t off = 0;; off += 4u * 1024 * 1024) {
        size_t chunk = n - off > 4u * 1024 * 1024 ? 4u * 1024 * 1024 : n - off;
        zs.next_in = (uint8_t *)(s + off);
        zs.avail_in = (uInt)chunk;
        int flush = (off + chunk >= n) ? Z_FINISH : Z_NO_FLUSH;
        for (;;) {
            zs.next_out = ob;
            zs.avail_out = (uInt)cap;
            int r = zng_deflate(&zs, flush);
            if (r == Z_STREAM_ERROR) zng_die();
            size_t got = cap - zs.avail_out;
            memcpy(d + produced, ob, got);
            produced += got;
            if (r == Z_STREAM_END) goto done;
            if (zs.avail_out != 0) break;
        }
        if (flush == Z_FINISH) break;
    }
done:
    zng_deflateEnd(&zs);
    free(ob);
    return produced;
}

static size_t base_zlibng_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    zng_stream zs;
    memset(&zs, 0, sizeof(zs));
    if (zng_inflateInit2(&zs, 15 + 16) != Z_OK) zng_die();
    zs.next_in = (uint8_t *)s;
    zs.avail_in = (uInt)n;
    size_t produced = 0;
    for (;;) {
        zs.next_out = d + produced;
        zs.avail_out = (uInt)(cap - produced);
        int r = zng_inflate(&zs, Z_NO_FLUSH);
        produced = cap - zs.avail_out;
        if (r == Z_STREAM_END) break;
        if (r != Z_OK) zng_die();
        if (zs.avail_in == 0 && zs.avail_out != 0) zng_die();
    }
    zng_inflateEnd(&zs);
    return produced;
}

/* bench.c 通过统一签名引用（函数指针） */
size_t bench_zng_c(const uint8_t *s, size_t n, int lvl, uint8_t *d, size_t cap) {
    return base_zlibng_c(s, n, lvl, d, cap);
}
size_t bench_zng_d(const uint8_t *s, size_t n, uint8_t *d, size_t cap) {
    return base_zlibng_d(s, n, d, cap);
}
