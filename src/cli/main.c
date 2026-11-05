/* omnic CLI（SW-CLI-001）：libomnic 的薄封装（INT-LIB-001）。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli/argp.h"
#include "cli/report.h"
#include "core/archive.h"
#include "core/fs.h"
#include "core/registry.h"
#include "core/util.h"

static void usage(FILE *out) {
    fputs("omnic - universal compressor (auto-selects the best codec per file type)\n\n"
          "usage:\n"
          "  omnic c   [options] <file...>    compress (auto codec unless -c)\n"
          "  omnic d   [options] <file...>    decompress (auto format by magic/ext; .omnic archives too)\n"
          "  omnic pack   [options] <dir>     pack a directory tree into a .omnic archive\n"
          "  omnic unpack [options] <archive> extract a .omnic archive\n"
          "  omnic analyze <file...>          show type/entropy/decision without compressing\n"
          "  omnic list                       list codecs and supported formats\n"
          "  omnic version | help\n\n"
          "options:\n"
          "  -p, --profile <v>   fast_compress | fast_decompress | best_ratio | balanced (default)\n"
          "  -c, --codec <name>  force a codec (see: omnic list)\n"
          "  -l, --level <n>     codec-native level\n"
          "  -T, --threads <n>   worker threads (zstd)\n"
          "  -o, --output-dir <dir>  pack: output archive path; unpack: output dir (same as -C)\n"
          "  -C, --dir <dir>     unpack: output root directory\n"
          "  -f, --force         overwrite output; compress incompressible data anyway\n"
          "  -k, --keep          keep the source file (default: remove after success)\n"
          "  -x, --explain       print the decision rationale\n"
          "  -q, --quiet\n"
          "  --per-file          pack: one block per file (fast single-file extract)\n"
          "  --no-skip           pack: compress already-compressed assets too\n"
          "  --block-size <MiB>  pack: solid block limit, 1..1024 (default 64)\n"
          "  --file <path>       unpack: extract only this entry (repeatable)\n"
          "  --list              unpack: list entries without extracting\n"
          "  --verify            unpack: verify archive without extracting\n",
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

/* .omnic 归档魔数探测（d 自动分流用） */
static int is_omnic_archive(const char *path) {
    FILE *f = omnic_fs_fopen(path, "rb");
    if (f == NULL) return 0;
    char m[8];
    size_t r = fread(m, 1, sizeof(m), f);
    fclose(f);
    return r == sizeof(m) && memcmp(m, "OMNICARC", 8) == 0;
}

/* unpack/d 的统一归档处理：--list / --verify / --file / 整树（SPEC §5.2） */
static int run_unpack(const char *arc, const omnic_args *a) {
    omnic_report rep;
    memset(&rep, 0, sizeof(rep));
    omnic_status st;
    if ((a->opts.flags & OMNIC_F_LIST) != 0) {
        st = omnic_arc_list(arc, &a->opts);
        if (st != OMNIC_OK) fprintf(stderr, "omnic: %s\n", omnic_last_error());
        return st == OMNIC_OK ? 0 : (int)st;
    }
    if ((a->opts.flags & OMNIC_F_VERIFY) != 0) {
        st = omnic_arc_verify(arc, &a->opts, &rep);
        if (st != OMNIC_OK && omnic_last_error()[0] != '\0')
            fprintf(stderr, "omnic: %s\n", omnic_last_error());
        return st == OMNIC_OK ? 0 : (int)st;
    }
    char def_dir[4096];
    snprintf(def_dir, sizeof(def_dir), "%s_out", arc);
    const char *dir = (a->opts.output_dir != NULL) ? a->opts.output_dir : def_dir;
    if (a->n_only > 0)
        st = omnic_unpack_files(arc, dir, a->only, (size_t)a->n_only, &a->opts, &rep);
    else
        st = omnic_unpack(arc, dir, &a->opts, &rep);
    if (st == OMNIC_OK) {
        printf("%s: extracted %llu bytes\n", arc, (unsigned long long)rep.out_bytes);
    } else if (omnic_last_error()[0] != '\0') {
        fprintf(stderr, "omnic: %s\n", omnic_last_error());
    }
    return st == OMNIC_OK ? 0 : (int)st;
}

static int cmd_pack_unpack(const omnic_args *a) {
    int rc = 0;
    for (int i = 0; i < a->nfiles; i++) {
        omnic_report rep;
        memset(&rep, 0, sizeof(rep));
        omnic_status st;
        if (strcmp(a->cmd, "pack") == 0) {
            char def_arc[4096];
            snprintf(def_arc, sizeof(def_arc), "%s.omnic", a->files[i]);
            const char *arc = (a->opts.output_dir != NULL) ? a->opts.output_dir : def_arc;
            st = omnic_pack(a->files[i], arc, &a->opts, &rep);
        } else {
            rc = run_unpack(a->files[i], a);
            continue;
        }
        if (st == OMNIC_OK) {
            printf("%s: %llu -> %llu bytes (%.2f%%), %.2fs\n", a->files[i],
                   (unsigned long long)rep.in_bytes, (unsigned long long)rep.out_bytes,
                   rep.in_bytes ? 100.0 * rep.ratio : 0.0, rep.seconds);
        } else {
            fprintf(stderr, "omnic: %s\n", omnic_last_error());
            if ((int)st > rc) rc = (int)st;
        }
    }
    return rc;
}

int main(int argc, char **argv) {
    omnic_args a;
    omnic_report rep;
    int rc = (int)OMNIC_OK;

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

    if (strcmp(a.cmd, "pack") == 0 || strcmp(a.cmd, "unpack") == 0) return cmd_pack_unpack(&a);

    for (int i = 0; i < a.nfiles; i++) {
        omnic_status st;
        memset(&rep, 0, sizeof(rep));
        if (strcmp(a.cmd, "c") == 0) {
            st = omnic_compress(a.files[i], &a.opts, &rep);
        } else if (strcmp(a.cmd, "d") == 0) {
            if (is_omnic_archive(a.files[i])) {
                /* .omnic 归档自动分流（SPEC §5.2）："omnic d 什么都能解"心智 */
                rc = run_unpack(a.files[i], &a);
                continue;
            }
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
            if ((int)st > rc) rc = (int)st;
        }
    }
    free(a.files);
    return rc;
}
