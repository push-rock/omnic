/* fuzz 配套：生成合法 lzvn 样本（bvxn 容器 + lzfse 库内 lzvn 编码）。
 * 用法：fuzz_lzvn_gen <out.lzvn> <ref_out> [size]
 * 结构同 tests/test_core.c test_lzvn_decode，仅为 fuzz 提供合法起点。 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* vendored lzfse 暴露的 lzvn 编码入口（声明见 test_core.c 同源） */
size_t lzvn_encode_scratch_size(void);
size_t lzvn_encode_buffer(void *dst, size_t dst_size, const void *src, size_t src_size, void *work);

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <out.lzvn> <ref_out> [size]\n", argv[0]);
        return 2;
    }
    size_t n = (argc > 3) ? (size_t)strtoul(argv[3], NULL, 10) : 60000;
    uint8_t *buf = (uint8_t *)malloc(n);
    size_t cap = n + n / 16 + 64;
    uint8_t *pay = (uint8_t *)malloc(cap);
    void *work = malloc(lzvn_encode_scratch_size());
    if (buf == NULL || pay == NULL || work == NULL) return 1;

    for (size_t i = 0; i < n; i++)
        buf[i] = (i % 9 == 0) ? (uint8_t)(i >> 4) : (uint8_t)('a' + (int)(i % 26));
    size_t plen = lzvn_encode_buffer(pay, cap, buf, n, work);
    if (plen == 0 || plen >= n) {
        fprintf(stderr, "encode failed\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "wb");
    FILE *r = fopen(argv[2], "wb");
    if (f == NULL || r == NULL) return 1;
    uint32_t hdr[3] = {0x6E787662u /* "bvxn" */, (uint32_t)n, (uint32_t)plen};
    fwrite(hdr, 4, 3, f);
    fwrite(pay, 1, plen, f);
    uint32_t tail = 0x24787662u /* "bvx$" */;
    fwrite(&tail, 4, 1, f);
    fwrite(buf, 1, n, r);
    fclose(f);
    fclose(r);
    printf("wrote %s (payload %zu + 16 hdr/tail) and %s (%zu bytes)\n", argv[1], plen, argv[2], n);
    return 0;
}
