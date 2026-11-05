/* gamepack - 游戏包 per-file 基准（M6 外部用例：vs Oodle Kraken）
 *
 * 协议：递归枚举 root 全部文件，逐文件 omnic_compress（决策器 per-file 选
 * codec），skip 类按 passthrough 记账（原样字节，模拟打包器 raw chunk）；
 * 每文件 R 轮解压取最快轮（与 bench 铁律一致），末轮后校验字节一致。
 * 产物名 = 输入名 + codec 扩展（output_dir=NULL 落输入旁），按唯一序号
 * 前缀配对，避免驱动侧维护 codec→扩展映射。
 * 指标两层：全量（含 passthrough）与可压子集。吞吐两行：纯 codec
 * （Σrep.seconds，与外部基准同口径）与墙钟（含采样决策与 I/O）。
 *
 * 用法: gamepack <root> [--profile bal|br|fc|fd] [--codec C --level N]
 *        [--rounds R] [--out DIR] [--csv PATH]
 * --codec 参照模式自动强压全部文件（skip 类含入），对齐 Kraken 整包
 * 压缩语义。注意 --out 指向的目录会被清空重建。
 */
#include <direct.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <windows.h>

#include "omnic.h"

#define GP_PATH 4096 /* windows.h 抢先定义 PATH_MAX=260，独立于平台宏 */

typedef struct {
    char path[GP_PATH];
    char name[280]; /* {6位序号}_{基名}：work 内唯一且保留扩展名供决策 */
    char codec[16]; /* pass 1 记录，pass 2 解压侧归账 */
    int klass;      /* 决策类（omnic_class），per-class 统计用 */
    unsigned long long bytes;
} file_entry;

typedef struct {
    char codec[24];
    unsigned long long in_bytes, out_bytes, dec_bytes;
    double comp_sec, dec_min;
    size_t n_files;
} codec_agg;

typedef struct {
    unsigned long long in_bytes, out_bytes;
    size_t n_files;
} class_agg;

static file_entry *g_files = NULL;
static size_t g_nfiles = 0, g_cap = 0;
static codec_agg g_agg[24];
static size_t g_nagg = 0;
static class_agg g_cls[16]; /* omnic_class 枚举值 0..15 足够 */

static codec_agg *agg_get(const char *codec) {
    for (size_t i = 0; i < g_nagg; i++)
        if (strcmp(g_agg[i].codec, codec) == 0) return &g_agg[i];
    if (g_nagg >= sizeof(g_agg) / sizeof(g_agg[0])) return NULL;
    memset(&g_agg[g_nagg], 0, sizeof(codec_agg));
    snprintf(g_agg[g_nagg].codec, sizeof(g_agg[0].codec), "%s", codec);
    return &g_agg[g_nagg++];
}

static void die_mem(void) {
    fprintf(stderr, "gamepack: out of memory\n");
    exit(3);
}

/* ---- 递归枚举 ---- */
static void add_file(const char *path, unsigned long long bytes) {
    if (g_nfiles == g_cap) {
        g_cap = g_cap ? g_cap * 2 : 1024;
        g_files = (file_entry *)realloc(g_files, g_cap * sizeof(file_entry));
        if (g_files == NULL) die_mem();
    }
    file_entry *e = &g_files[g_nfiles];
    const char *base = strrchr(path, '/');
    const char *base2 = strrchr(path, '\\');
    if (base2 != NULL && (base == NULL || base2 > base)) base = base2;
    base = (base != NULL) ? base + 1 : path;
    if (strlen(base) >= sizeof(e->name) - 8) return; /* 超长基名跳过，数量极少 */
    snprintf(e->name, sizeof(e->name), "%06zu_%s", g_nfiles, base);
    snprintf(e->path, sizeof(e->path), "%s", path);
    snprintf(e->codec, sizeof(e->codec), "%s", OMNIC_SKIP_CODEC);
    e->bytes = bytes;
    g_nfiles++;
}

static void walk(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char sub[GP_PATH];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >= (int)sizeof(sub)) continue;
        struct stat st;
        if (stat(sub, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            walk(sub);
        else if (st.st_size > 0)
            add_file(sub, (unsigned long long)st.st_size);
    }
    closedir(d);
}

static void rm_rf(const char *dir) {
    DIR *d = opendir(dir);
    if (d == NULL) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char sub[GP_PATH];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >= (int)sizeof(sub)) continue;
        struct stat st;
        if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
            rm_rf(sub);
        } else {
            remove(sub);
        }
    }
    closedir(d);
    rmdir(dir);
}

/* ---- 文件工具 ---- */
static int copy_file(const char *from, const char *to) {
    FILE *a = fopen(from, "rb"), *b = fopen(to, "wb");
    if (a == NULL || b == NULL) {
        if (a != NULL) fclose(a);
        if (b != NULL) fclose(b);
        return -1;
    }
    char buf[65536];
    size_t n;
    int bad = 0;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, n, b) != n) {
            bad = 1;
            break;
        }
    if (ferror(a)) bad = 1;
    fclose(a);
    if (fclose(b) != 0) bad = 1;
    return bad ? -1 : 0;
}

static int files_equal(const char *x, const char *y) {
    FILE *a = fopen(x, "rb"), *b = fopen(y, "rb");
    if (a == NULL || b == NULL) {
        if (a != NULL) fclose(a);
        if (b != NULL) fclose(b);
        return 0;
    }
    char ba[65536], bb[65536];
    int eq = 1;
    for (;;) {
        size_t na = fread(ba, 1, sizeof(ba), a), nb = fread(bb, 1, sizeof(bb), b);
        if (na != nb || (na > 0 && memcmp(ba, bb, na) != 0)) {
            eq = 0;
            break;
        }
        if (na == 0) break;
    }
    fclose(a);
    fclose(b);
    return eq;
}

static double now_sec(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: gamepack <root> [--profile bal|br|fc|fd] [--codec C --level N]\n"
                        "                   [--rounds R] [--out DIR] [--csv PATH]\n");
        return 1;
    }
    const char *root = argv[1];
    const char *work = "gp_work";
    const char *csv_path = NULL;
    const char *perfile_path = NULL;
    omnic_options o;
    memset(&o, 0, sizeof(o));
    o.profile = OMNIC_PROFILE_BALANCED;
    /* 压缩不带 FORCE（保留 skip 语义）；解压要 FORCE（多轮覆盖产物）。
     * --codec 参照模式例外：强压全部文件对齐 Kraken 整包压缩语义。
     * 判定须在参数解析后（o.codec 此前为 NULL）。 */
    unsigned comp_flags = OMNIC_F_KEEP | OMNIC_F_QUIET;
    unsigned dec_flags = OMNIC_F_KEEP | OMNIC_F_FORCE | OMNIC_F_QUIET;
    int rounds = 3;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            const char *p = argv[++i];
            o.profile = (strcmp(p, "fc") == 0) ? OMNIC_PROFILE_FAST_COMPRESS
                        : (strcmp(p, "fd") == 0) ? OMNIC_PROFILE_FAST_DECOMPRESS
                        : (strcmp(p, "br") == 0) ? OMNIC_PROFILE_BEST_RATIO
                                                 : OMNIC_PROFILE_BALANCED;
        } else if (strcmp(argv[i], "--codec") == 0 && i + 1 < argc) {
            o.codec = argv[++i];
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            o.level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rounds") == 0 && i + 1 < argc) {
            rounds = atoi(argv[++i]);
            if (rounds < 1) rounds = 1;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            work = argv[++i];
        } else if (strcmp(argv[i], "--csv") == 0 && i + 1 < argc) {
            csv_path = argv[++i];
        } else if (strcmp(argv[i], "--perfile") == 0 && i + 1 < argc) {
            perfile_path = argv[++i];
        }
    }
    if (o.codec != NULL) comp_flags |= OMNIC_F_FORCE; /* 参照模式强压全部（含 skip 类） */

    FILE *pf = NULL; /* --perfile：逐文件决策清单（solid 分桶实验用） */
    if (perfile_path != NULL) {
        pf = fopen(perfile_path, "w");
        if (pf == NULL) {
            fprintf(stderr, "gamepack: cannot open %s\n", perfile_path);
            return 1;
        }
        fprintf(pf, "path,class,codec,level,skipped,in_bytes,out_bytes\n");
    }

    walk(root);
    unsigned long long total_in = 0;
    for (size_t i = 0; i < g_nfiles; i++) total_in += g_files[i].bytes;
    printf("gamepack: %zu files, %llu bytes under %s\n", g_nfiles, total_in, root);
    if (g_nfiles == 0) return 2;

    if (strlen(work) > 480) {
        fprintf(stderr, "gamepack: --out path too long\n");
        return 1;
    }
    char dir_ref[512];
    snprintf(dir_ref, sizeof(dir_ref), "%s/ref", work);
    rm_rf(work);
    _mkdir(work);
    _mkdir(dir_ref);

    /* pass 0: 唯一化复制（保扩展名供决策器路由） */
    for (size_t i = 0; i < g_nfiles; i++) {
        char dst[GP_PATH];
        snprintf(dst, sizeof(dst), "%s/%s", work, g_files[i].name);
        if (copy_file(g_files[i].path, dst) != 0) {
            fprintf(stderr, "gamepack: copy failed: %s\n", g_files[i].path);
            return 3;
        }
    }

    /* pass 1: 逐文件压缩（决策器 per-file；产物落输入旁） */
    unsigned long long pass_out = 0, comp_in = 0;
    double comp_sec_sum = 0.0, wall0 = now_sec();
    for (size_t i = 0; i < g_nfiles; i++) {
        char src[GP_PATH];
        snprintf(src, sizeof(src), "%s/%s", work, g_files[i].name);
        omnic_report rep;
        o.flags = comp_flags;
        omnic_status st = omnic_compress(src, &o, &rep);
        if (st != OMNIC_OK) {
            fprintf(stderr, "gamepack: compress failed on %s: %s\n", g_files[i].path,
                    omnic_status_str(st));
            return 3;
        }
        const char *cname = rep.skipped ? "passthrough" : rep.codec;
        snprintf(g_files[i].codec, sizeof(g_files[i].codec), "%s", cname);
        g_files[i].klass = (int)rep.klass;
        if (rep.klass >= 0 && rep.klass < (int)(sizeof(g_cls) / sizeof(g_cls[0]))) {
            g_cls[rep.klass].n_files++;
            g_cls[rep.klass].in_bytes += g_files[i].bytes;
            g_cls[rep.klass].out_bytes += rep.skipped ? g_files[i].bytes : rep.out_bytes;
        }
        if (pf != NULL)
            fprintf(pf, "%s,%d,%s,%d,%d,%llu,%llu\n", g_files[i].path, (int)rep.klass,
                    rep.skipped ? "skip" : rep.codec, rep.level, rep.skipped ? 1 : 0,
                    g_files[i].bytes, rep.skipped ? g_files[i].bytes : rep.out_bytes);
        codec_agg *a = agg_get(cname);
        if (a != NULL) {
            a->n_files++;
            a->in_bytes += g_files[i].bytes;
            if (rep.skipped) {
                a->out_bytes += g_files[i].bytes;
            } else {
                a->out_bytes += rep.out_bytes;
                a->comp_sec += rep.seconds;
                comp_in += g_files[i].bytes;
                comp_sec_sum += rep.seconds;
                pass_out += rep.out_bytes;
            }
        }
    }
    double wall1 = now_sec();

    /* pass 2: 解压轮（产物 = 唯一名 + '.' + 扩展；输入复件无后缀可区分） */
    unsigned long long dec_bytes = 0;
    double dec_min_sum = 0.0;
    size_t n_pass = 0;
    for (size_t i = 0; i < g_nfiles; i++) {
        char src[GP_PATH], ref[GP_PATH], prod[GP_PATH];
        snprintf(src, sizeof(src), "%s/%s", work, g_files[i].name);
        snprintf(ref, sizeof(ref), "%s/%s", dir_ref, g_files[i].name);
        if (strcmp(g_files[i].codec, "passthrough") == 0) continue;

        prod[0] = '\0';
        DIR *d = opendir(work);
        if (d == NULL) return 3;
        struct dirent *ent;
        size_t nlen = strlen(g_files[i].name);
        while ((ent = readdir(d)) != NULL) {
            if (strncmp(ent->d_name, g_files[i].name, nlen) == 0 &&
                ent->d_name[nlen] == '.') {
                snprintf(prod, sizeof(prod), "%s/%s", work, ent->d_name);
                break;
            }
        }
        closedir(d);
        if (prod[0] == '\0') continue; /* 防御：压缩成功必有产物 */

        if (copy_file(src, ref) != 0) return 3; /* 解压输出将覆盖输入位置 */
        double best = 1e30;
        for (int r = 0; r < rounds; r++) {
            omnic_report drep;
            o.flags = dec_flags;
            omnic_status st = omnic_decompress(prod, &o, &drep);
            if (st != OMNIC_OK) {
                fprintf(stderr, "gamepack: decompress failed on %s: %s\n", prod,
                        omnic_status_str(st));
                return 3;
            }
            if (drep.seconds < best) best = drep.seconds;
        }
        if (!files_equal(src, ref)) {
            fprintf(stderr, "gamepack: roundtrip mismatch on %s\n", g_files[i].path);
            return 3;
        }
        codec_agg *a = agg_get(g_files[i].codec);
        if (a != NULL) {
            a->dec_bytes += g_files[i].bytes;
            a->dec_min += best;
        }
        dec_bytes += g_files[i].bytes;
        dec_min_sum += best;
        n_pass++;
        remove(prod);
    }

    double mib = 1048576.0;
    unsigned long long stored = pass_out + (total_in - comp_in);
    printf("\n== gamepack result (%s%s, rounds=%d) ==\n",
           o.codec != NULL ? o.codec : omnic_profile_str(o.profile),
           o.codec != NULL ? " (forced)" : "", rounds);
    printf("files: %zu (compressed %zu, passthrough %zu)\n", g_nfiles, n_pass,
           g_nfiles - n_pass);
    printf("overall: %llu -> %llu bytes, ratio %.4f%%\n", total_in, stored,
           100.0 * (double)stored / (double)total_in);
    printf("compressed subset: %llu bytes | dec %.1f MiB/s (pure codec, min-of-%d)\n",
           dec_bytes, dec_min_sum > 0 ? (double)dec_bytes / mib / dec_min_sum : 0.0, rounds);
    printf("compress: %.1f MiB/s pure codec | %.1f MiB/s wall (incl. sampling+IO)\n",
           comp_sec_sum > 0 ? (double)comp_in / mib / comp_sec_sum : 0.0,
           wall1 - wall0 > 0 ? (double)comp_in / mib / (wall1 - wall0) : 0.0);
    printf("\nper-codec (compressed files):\n");
    for (size_t i = 0; i < g_nagg; i++) {
        codec_agg *a = &g_agg[i];
        if (a->n_files == 0) continue;
        printf("  %-20s files %6zu  in %9.2f MiB  out %8.2f MiB  ratio %7.4f%%"
               "  dec %7.1f MiB/s\n",
               a->codec, a->n_files, (double)a->in_bytes / mib, (double)a->out_bytes / mib,
               a->in_bytes > 0 ? 100.0 * (double)a->out_bytes / (double)a->in_bytes : 0.0,
               a->dec_min > 0 ? (double)a->dec_bytes / mib / a->dec_min : 0.0);
    }
    printf("\nper-class (decisioner):\n");
    for (int k = 0; k < (int)(sizeof(g_cls) / sizeof(g_cls[0])); k++) {
        if (g_cls[k].n_files == 0) continue;
        printf("  %-20s files %6zu  in %9.2f MiB  out %8.2f MiB\n", omnic_class_str((omnic_class)k),
               g_cls[k].n_files, (double)g_cls[k].in_bytes / mib,
               (double)g_cls[k].out_bytes / mib);
    }
    if (csv_path != NULL) {
        FILE *c = fopen(csv_path, "w");
        if (c != NULL) {
            fprintf(c, "files,compressed,passthrough,in_bytes,out_bytes,ratio_pct,"
                       "dec_mibps_pure,comp_mibps_pure\n");
            fprintf(c, "%zu,%zu,%zu,%llu,%llu,%.4f,%.2f,%.2f\n", g_nfiles, n_pass,
                    g_nfiles - n_pass, total_in, stored,
                    100.0 * (double)stored / (double)total_in,
                    dec_min_sum > 0 ? (double)dec_bytes / mib / dec_min_sum : 0.0,
                    comp_sec_sum > 0 ? (double)comp_in / mib / comp_sec_sum : 0.0);
            fclose(c);
        }
    }
    if (pf != NULL) fclose(pf);
    rm_rf(work);
    return 0;
}
