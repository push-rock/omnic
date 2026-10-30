/* omnic CLI（SW-CLI-001）：libomnic 的薄封装（INT-LIB-001）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/argp.h"
#include "cli/report.h"
#include "core/registry.h"
#include "core/util.h"

static void usage(FILE *out) {
    fputs("omnic - universal compressor (auto-selects the best codec per file type)\n\n"
          "usage:\n"
          "  omnic c   [options] <file...>    compress (auto codec unless -c)\n"
          "  omnic d   [options] <file...>    decompress (auto format by magic/ext)\n"
          "  omnic analyze <file...>          show type/entropy/decision without compressing\n"
          "  omnic list                       list codecs and supported formats\n"
          "  omnic version | help\n\n"
          "options:\n"
          "  -p, --profile <v>   fast_compress | fast_decompress | best_ratio | balanced (default)\n"
          "  -c, --codec <name>  force a codec (see: omnic list)\n"
          "  -l, --level <n>     codec-native level\n"
          "  -T, --threads <n>   worker threads (zstd)\n"
          "  -o, --output-dir <dir>\n"
          "  -f, --force         overwrite output; compress incompressible data anyway\n"
          "  -k, --keep          keep the source file (default: remove after success)\n"
          "  -x, --explain       print the decision rationale\n"
          "  -q, --quiet\n",
          out);
}

static int cmd_list(void) {
    printf("codecs (compress+decompress):\n");
    for (size_t i = 0;; i++) {
        const omnic_codec_ops *ops = omnic_codec_at(i);
        if (ops == NULL) break;
        if (!ops->can_compress) continue; /* 仅解码条目归到下方段落 */
        printf("  %-10s levels %3d..%-3d default %-3d ext %-6s format %s\n", ops->name,
               ops->min_level, ops->max_level, ops->default_level, ops->native_ext,
               omnic_fmt_str(ops->fmt));
    }
    printf("decode-only codecs:\n");
    for (size_t i = 0;; i++) {
        const omnic_codec_ops *ops = omnic_codec_at(i);
        if (ops == NULL) break;
        if (ops->can_compress) continue;
        printf("  %-10s ext %-6s format %s\n", ops->name, ops->native_ext,
               omnic_fmt_str(ops->fmt));
    }
    printf("formats recognized on decompress (magic first, ext fallback): "
           "gzip zlib deflate zstd xz lzma lzip bzip2 lz4 brotli snappy compress(.Z) "
           "lzfse lzvn%s%s\n",
           omnic_codec_for_fmt(OMNIC_FMT_LZHAM) ? " lzham" : "",
           omnic_codec_for_fmt(OMNIC_FMT_LZAV) ? " lzav" : "");
    return 0;
}

int main(int argc, char **argv) {
    omnic_args a;
    omnic_report rep;
    int rc = 0;

    if (omnic_parse_args(argc, argv, &a) != 0) {
        usage(stderr);
        return OMNIC_E_USAGE;
    }

    if (strcmp(a.cmd, "version") == 0) {
        printf("omnic %s\n", omnic_version_str());
        return 0;
    }
    if (strcmp(a.cmd, "help") == 0) {
        usage(stdout);
        return 0;
    }
    if (strcmp(a.cmd, "list") == 0) return cmd_list();

    for (int i = 0; i < a.nfiles; i++) {
        omnic_status st;
        memset(&rep, 0, sizeof(rep));
        if (strcmp(a.cmd, "c") == 0) {
            st = omnic_compress(a.files[i], &a.opts, &rep);
        } else if (strcmp(a.cmd, "d") == 0) {
            st = omnic_decompress(a.files[i], &a.opts, &rep);
        } else { /* analyze */
            st = omnic_analyze(a.files[i], &rep);
            if (st == OMNIC_OK) {
                if (!(a.opts.flags & OMNIC_F_QUIET)) {
                    printf("%s: %s | entropy %.2f | -> %s level %d\n", a.files[i],
                           omnic_class_str(rep.klass), rep.entropy, rep.codec, rep.level);
                    if (rep.reason[0] != '\0') printf("    %s\n", rep.reason);
                }
                continue;
            }
        }
        if (st == OMNIC_OK) {
            omnic_print_report(a.files[i], &rep, &a.opts);
        } else {
            fprintf(stderr, "omnic: %s\n", omnic_last_error());
            if (st > rc) rc = st;
        }
    }
    free(a.files);
    return rc;
}
