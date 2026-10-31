#include "core/dispatcher.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "codec/codec.h"
#include "core/blockio.h"
#include "core/registry.h"
#include "core/util.h"
#include "detect/entropy.h"
#include "detect/feature.h"
#include "detect/filetype.h"
#include "detect/magic.h"
#include "policy/policy.h"

#define OMNIC_TMP_SUFFIX ".omnic-tmp"

typedef struct {
    FILE *in;
    FILE *out;
    char *out_path;
    char *tmp_path;
    omnic_reader reader;
    omnic_writer writer;
    bool out_open;
} omnic_files;

static void files_close(omnic_files *f, bool rollback) {
    if (f->out_open && f->out != NULL) fclose(f->out);
    if (rollback && f->tmp_path != NULL) remove(f->tmp_path);
    if (f->in != NULL) fclose(f->in);
    free(f->tmp_path);
    free(f->out_path);
    f->out = f->in = NULL;
    f->out_path = f->tmp_path = NULL;
}

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return false;
    fclose(f);
    return true;
}

/* Windows 的 rename() 不覆盖已存在目标（-f 已放行的覆盖场景）：先删后改名 */
static omnic_status finalize_rename(const char *tmp_path, const char *out_path) {
    if (rename(tmp_path, out_path) == 0) return OMNIC_OK;
    remove(out_path);
    return rename(tmp_path, out_path) == 0 ? OMNIC_OK : OMNIC_E_SYSTEM;
}

/* 采样+分类+决策（压缩路径 1-2 步，SYS-CMP-001/002）。
 * 二期分层采样（PHASE2_DECIDER_DESIGN §3.2）：头/中/尾三段各 256KiB，
 * 特征向量 φ 驱动细分类 γ；决策后 rewind 交还 codec。直接走 FILE*
 * 而非 reader：三段定位需要 fseek，且不污染 reader 的 total 计数。 */
typedef struct {
    omnic_class klass;
    double entropy;
    omnic_features feats;
    const omnic_policy_entry *pe;
    omnic_policy_entry override; /* 试压胜者的表行副本（pe 指向它时生效） */
    char reason_buf[96];         /* override.reason 的可写缓冲 */
    uint8_t *seg_base;           /* 三段采样缓冲（决策方持有，调用方 free） */
    const uint8_t *trial;        /* 中段（试压对象） */
    size_t trial_len;
    long file_size;
} omnic_decision;

/* 三值中位数（小写首字母避免与 math 库宏冲突） */
static double med3(double a, double b, double c) {
    if (a > b) {
        if (b > c) return b;
        return a > c ? c : a;
    }
    if (a > c) return a;
    return b > c ? c : b;
}

static omnic_status decide(FILE *f, const char *path, const omnic_options *o, omnic_decision *d) {
    uint8_t *seg = (uint8_t *)malloc(3 * OMNIC_SAMPLE_SEG);
    if (seg == NULL) return OMNIC_E_SYSTEM;

    long sz = -1;
    if (fseek(f, 0, SEEK_END) == 0) sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t hn = fread(seg, 1, OMNIC_SAMPLE_SEG, f);
    size_t mn = 0, tn = 0;
    /* >=3 段长才有分层意义；两段(头+尾)覆盖 [s,3s)；更短只有头段。
     * sz<0（fseek/ftell 失败，含 Win32 long 溢出的 >2GB 文件）退化为头段采样。 */
    if (sz >= (long)(3 * OMNIC_SAMPLE_SEG)) {
        if (fseek(f, sz / 2 - (long)(OMNIC_SAMPLE_SEG / 2), SEEK_SET) == 0)
            mn = fread(seg + OMNIC_SAMPLE_SEG, 1, OMNIC_SAMPLE_SEG, f);
        if (fseek(f, sz - (long)OMNIC_SAMPLE_SEG, SEEK_SET) == 0)
            tn = fread(seg + 2 * OMNIC_SAMPLE_SEG, 1, OMNIC_SAMPLE_SEG, f);
    } else if (sz >= (long)(2 * OMNIC_SAMPLE_SEG)) {
        if (fseek(f, sz - (long)OMNIC_SAMPLE_SEG, SEEK_SET) == 0)
            tn = fread(seg + 2 * OMNIC_SAMPLE_SEG, 1, OMNIC_SAMPLE_SEG, f);
    }
    fseek(f, 0, SEEK_SET);

    omnic_feature_extract(seg, hn, seg + OMNIC_SAMPLE_SEG, mn, seg + 2 * OMNIC_SAMPLE_SEG, tn,
                          &d->feats);
    /* 熵取三段中位数：与 RANDOM 判定同源，抗单段误判（如文本尾附 base64 段） */
    d->entropy = med3(d->feats.entropy_h, d->feats.entropy_m, d->feats.entropy_t);
    d->seg_base = seg; /* 调用方（含 analyze 路径）负责 free */
    d->trial = seg + OMNIC_SAMPLE_SEG;
    d->trial_len = mn;
    d->file_size = sz;

    d->klass = omnic_classify_ext(path);
    if (d->klass == OMNIC_CLS_UNKNOWN && d->entropy >= OMNIC_ENTROPY_SKIP) {
        d->klass = OMNIC_CLS_RANDOM; /* SW-DET-003 */
    } else {
        d->klass = omnic_refine_class(d->klass, &d->feats); /* γ 细分（PHASE2 §3.4） */
    }
    d->pe = omnic_policy_lookup(d->klass, o->profile);
    return OMNIC_OK;
}

/* FC 档候选试压（PHASE2 §3.5）：对中段 256KiB 实测两候选的 (t, ρ)，
 * 预算内取 ρ 优者覆写表行。mem 后端保证时序不含磁盘 IO。 */
static const omnic_codec_ops *trial_codec(const char *name) {
    const omnic_codec_ops *ops = omnic_codec_find(name);
    return (ops != NULL && ops->can_compress) ? ops : NULL;
}

static omnic_status trial_run(const omnic_codec_ops *ops, int level, const uint8_t *src,
                              size_t n, double *t_out, size_t *len_out) {
    omnic_reader r;
    omnic_writer w;
    omnic_reader_init_mem(&r, src, n);
    omnic_writer_init_mem(&w);
    double t0 = omnic_now_sec();
    omnic_status st = ops->compress(&r, &w, level, 1);
    double t1 = omnic_now_sec();
    *t_out = t1 - t0;
    *len_out = (size_t)omnic_writer_total(&w);
    free(omnic_writer_mem_drain(&w, NULL));
    omnic_reader_fini(&r);
    return st;
}

static void trial_pick(omnic_decision *d) {
    const omnic_codec_ops *c1 = trial_codec(d->pe->codec);
    const omnic_codec_ops *c2 = trial_codec(d->pe->codec2);
    double t1 = 0, t2 = 0;
    size_t l1 = 0, l2 = 0;
    if (c1 == NULL || c2 == NULL || c1 == c2) return;
    omnic_status s1 = trial_run(c1, d->pe->level, d->trial, d->trial_len, &t1, &l1);
    omnic_status s2 = trial_run(c2, d->pe->level2, d->trial, d->trial_len, &t2, &l2);
    if (s1 != OMNIC_OK && s2 != OMNIC_OK) return;
    /* 预算内 ρ 优者胜（PHASE2 预注册：argmax ρ s.t. t ≤ 50ms；全超 → 默认 c1） */
    bool use2 = (s2 == OMNIC_OK && t2 <= OMNIC_TRIAL_BUDGET_SEC &&
                 (s1 != OMNIC_OK || t1 > OMNIC_TRIAL_BUDGET_SEC || l2 < l1));
    d->override = *d->pe;
    if (use2) {
        d->override.codec = d->pe->codec2;
        d->override.level = d->pe->level2;
    }
    /* reason 走独立缓冲（表内 reason 是字面量，不可写）；ρ 用百分位显示 */
    double r1 = (d->trial_len > 0) ? 100.0 * (double)l1 / (double)d->trial_len : 0.0;
    double r2 = (d->trial_len > 0) ? 100.0 * (double)l2 / (double)d->trial_len : 0.0;
    snprintf(d->reason_buf, sizeof(d->reason_buf), "trial 256KiB: %s %.1f%% vs %s %.1f%%",
             d->pe->codec, r1, d->pe->codec2, r2);
    d->override.reason = d->reason_buf;
    d->pe = &d->override;
}

static const omnic_codec_ops *pick_codec(const omnic_options *o, const omnic_policy_entry *pe,
                                         int *level_out, omnic_report *rep) {
    const omnic_codec_ops *ops;
    if (o->codec != NULL) {
        ops = omnic_codec_find(o->codec);
        if (ops == NULL || !ops->can_compress) {
            omnic_set_error("unknown codec '%s' (see: omnic list)", o->codec);
            return NULL;
        }
        *level_out = (o->level > 0) ? o->level : ops->default_level;
        snprintf(rep->reason, sizeof(rep->reason), "manual: %s", o->codec);
    } else if (pe != NULL && !omnic_policy_is_skip(pe)) {
        ops = omnic_codec_find(pe->codec);
        if (ops == NULL || !ops->can_compress) ops = omnic_codec_find("zstd"); /* 决策表 codec 未编入时兜底 */
        if (ops == NULL) {
            omnic_set_error("no usable codec compiled in");
            return NULL;
        }
        *level_out = (o->level > 0) ? o->level : pe->level;
        snprintf(rep->reason, sizeof(rep->reason), "%s", pe->reason);
    } else if (pe != NULL && omnic_policy_is_skip(pe)) {
        /* 强压不可压缩数据：退回 UNKNOWN 行为 */
        const omnic_policy_entry *fb = omnic_policy_lookup(OMNIC_CLS_UNKNOWN, o->profile);
        ops = omnic_codec_find(fb != NULL ? fb->codec : "zstd");
        if (ops == NULL) {
            omnic_set_error("no usable codec compiled in");
            return NULL;
        }
        *level_out = (o->level > 0) ? o->level : (fb != NULL ? fb->level : ops->default_level);
        snprintf(rep->reason, sizeof(rep->reason), "forced over skip");
    } else {
        ops = omnic_codec_find("zstd");
        if (ops == NULL) {
            omnic_set_error("no usable codec compiled in");
            return NULL;
        }
        *level_out = ops->default_level;
        snprintf(rep->reason, sizeof(rep->reason), "fallback");
    }
    if (*level_out < ops->min_level) *level_out = ops->min_level;
    if (*level_out > ops->max_level) *level_out = ops->max_level;
    return ops;
}

static void fill_report(omnic_report *rep, const char *codec_name, int level, omnic_class klass,
                        double entropy, uint64_t in, uint64_t out, double sec) {
    snprintf(rep->codec, sizeof(rep->codec), "%s", codec_name);
    rep->level = level;
    rep->klass = klass;
    rep->entropy = entropy;
    rep->in_bytes = in;
    rep->out_bytes = out;
    rep->seconds = sec;
    rep->ratio = (in > 0) ? (double)out / (double)in : 0.0;
    rep->mbps = (sec > 0.0) ? (double)in / 1e6 / sec : 0.0;
}

omnic_status omnic_run_compress(const char *path, const omnic_options *o, omnic_report *rep) {
    omnic_files f;
    omnic_decision d;
    omnic_status ret = OMNIC_OK;
    const omnic_codec_ops *ops;
    int level = 0;
    double t0, t1;

    memset(&f, 0, sizeof(f));
    memset(&d, 0, sizeof(d));

    f.in = fopen(path, "rb");
    if (f.in == NULL) {
        omnic_set_error("cannot open '%s': %s", path, strerror(errno));
        return OMNIC_E_SYSTEM;
    }
    omnic_reader_init(&f.reader, f.in);

    ret = decide(f.in, path, o, &d);
    if (ret != OMNIC_OK) goto fail;

    /* FC 档试压（PHASE2 §3.5）：≥32MiB 且表给了第二候选才触发；
     * 试压后 d.pe 可能指向 d.override（栈内副本） */
    if (o->codec == NULL && o->profile == OMNIC_PROFILE_FAST_COMPRESS && d.pe != NULL &&
        !omnic_policy_is_skip(d.pe) && d.pe->codec2 != NULL && d.trial_len > 0 &&
        d.file_size >= (long)OMNIC_TRIAL_MIN) {
        trial_pick(&d);
    }

    rep->klass = d.klass;
    rep->entropy = d.entropy;

    /* 跳过路径（SYS-CMP-003 / 假设 A1） */
    if ((omnic_policy_is_skip(d.pe) || d.klass == OMNIC_CLS_ALREADY_COMPRESSED ||
         d.klass == OMNIC_CLS_RANDOM) &&
        !(o->flags & OMNIC_F_FORCE)) {
        rep->skipped = true;
        snprintf(rep->codec, sizeof(rep->codec), "%s", OMNIC_SKIP_CODEC);
        snprintf(rep->reason, sizeof(rep->reason), "%s: %s (entropy %.2f)",
                 omnic_class_str(d.klass), "use --force to compress anyway", d.entropy);
        fill_report(rep, OMNIC_SKIP_CODEC, 0, d.klass, d.entropy, 0, 0, 0.0);
        files_close(&f, false);
        free(d.seg_base);
        return OMNIC_OK;
    }

    ops = pick_codec(o, d.pe, &level, rep);
    if (ops == NULL) {
        ret = OMNIC_E_USAGE;
        goto fail;
    }

    f.out_path = omnic_join_ext(path, ops->native_ext, o->output_dir);
    if (f.out_path == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }
    if (file_exists(f.out_path) && !(o->flags & OMNIC_F_FORCE)) {
        omnic_set_error("output '%s' already exists (use --force)", f.out_path);
        ret = OMNIC_E_USAGE;
        goto fail;
    }
    {
        size_t n = strlen(f.out_path) + strlen(OMNIC_TMP_SUFFIX) + 1;
        f.tmp_path = (char *)malloc(n);
        if (f.tmp_path == NULL) {
            ret = OMNIC_E_SYSTEM;
            goto fail;
        }
        snprintf(f.tmp_path, n, "%s%s", f.out_path, OMNIC_TMP_SUFFIX);
    }
    f.out = fopen(f.tmp_path, "wb");
    if (f.out == NULL) {
        omnic_set_error("cannot create '%s': %s", f.tmp_path, strerror(errno));
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }
    f.out_open = true;
    omnic_writer_init(&f.writer, f.out);

    t0 = omnic_now_sec();
    ret = ops->compress(&f.reader, &f.writer, level, o->threads);
    t1 = omnic_now_sec();
    if (ret != OMNIC_OK) {
        omnic_set_error("%s: compress failed (%s)", path, omnic_status_str(ret));
        goto fail;
    }
    if (fflush(f.out) != 0 || fclose(f.out) != 0) {
        f.out_open = false;
        omnic_set_error("cannot write '%s': %s", f.tmp_path, strerror(errno));
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }
    f.out_open = false;
    if (finalize_rename(f.tmp_path, f.out_path) != OMNIC_OK) {
        omnic_set_error("cannot finalize '%s': %s", f.out_path, strerror(errno));
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }

    fill_report(rep, ops->name, level, d.klass, d.entropy, omnic_reader_total(&f.reader),
                omnic_writer_total(&f.writer), t1 - t0);
    rep->fmt = ops->fmt;

    files_close(&f, false);
    free(d.seg_base);
    if (!(o->flags & OMNIC_F_KEEP)) remove(path); /* 假设 A8：默认删源 */
    return OMNIC_OK;

fail:
    files_close(&f, true);
    free(d.seg_base);
    return ret;
}

omnic_status omnic_run_decompress(const char *path, const omnic_options *o, omnic_report *rep) {
    omnic_files f;
    omnic_status ret = OMNIC_OK;
    const omnic_codec_ops *ops;
    uint8_t head[OMNIC_MAGIC_SIZE];
    omnic_fmt_id fmt;
    double t0, t1;

    memset(&f, 0, sizeof(f));

    f.in = fopen(path, "rb");
    if (f.in == NULL) {
        omnic_set_error("cannot open '%s': %s", path, strerror(errno));
        return OMNIC_E_SYSTEM;
    }
    omnic_reader_init(&f.reader, f.in);
    {
        size_t n = omnic_reader_read(&f.reader, head, sizeof(head));
        omnic_reader_unread(&f.reader, head, n);
        fmt = omnic_detect(path, head, n);
    }
    if (fmt == OMNIC_FMT_UNKNOWN) {
        omnic_set_error("'%s': unrecognized compression format (see: omnic list)", path);
        ret = OMNIC_E_DATA;
        goto fail;
    }
    ops = omnic_codec_for_fmt(fmt);
    if (ops == NULL) {
        omnic_set_error("'%s': format %s recognized but no decoder compiled in", path,
                        omnic_fmt_str(fmt));
        ret = OMNIC_E_DATA;
        goto fail;
    }

    f.out_path = omnic_strip_ext(path);
    if (f.out_path == NULL) {
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }
    if (file_exists(f.out_path) && !(o->flags & OMNIC_F_FORCE)) {
        omnic_set_error("output '%s' already exists (use --force)", f.out_path);
        ret = OMNIC_E_USAGE;
        goto fail;
    }
    {
        size_t n = strlen(f.out_path) + strlen(OMNIC_TMP_SUFFIX) + 1;
        f.tmp_path = (char *)malloc(n);
        if (f.tmp_path == NULL) {
            ret = OMNIC_E_SYSTEM;
            goto fail;
        }
        snprintf(f.tmp_path, n, "%s%s", f.out_path, OMNIC_TMP_SUFFIX);
    }
    f.out = fopen(f.tmp_path, "wb");
    if (f.out == NULL) {
        omnic_set_error("cannot create '%s': %s", f.tmp_path, strerror(errno));
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }
    f.out_open = true;
    omnic_writer_init(&f.writer, f.out);

    t0 = omnic_now_sec();
    ret = ops->decompress(&f.reader, &f.writer);
    t1 = omnic_now_sec();
    if (ret != OMNIC_OK) {
        omnic_set_error("%s: decompress failed (%s)", path, omnic_status_str(ret));
        goto fail;
    }
    if (fflush(f.out) != 0 || fclose(f.out) != 0) {
        f.out_open = false;
        omnic_set_error("cannot write '%s': %s", f.tmp_path, strerror(errno));
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }
    f.out_open = false;
    if (finalize_rename(f.tmp_path, f.out_path) != OMNIC_OK) {
        omnic_set_error("cannot finalize '%s': %s", f.out_path, strerror(errno));
        ret = OMNIC_E_SYSTEM;
        goto fail;
    }

    fill_report(rep, ops->name, 0, OMNIC_CLS_UNKNOWN, 0.0, omnic_reader_total(&f.reader),
                omnic_writer_total(&f.writer), t1 - t0);
    rep->fmt = fmt;

    files_close(&f, false);
    if (!(o->flags & OMNIC_F_KEEP)) remove(path);
    return OMNIC_OK;

fail:
    files_close(&f, true);
    return ret;
}

omnic_status omnic_run_analyze(const char *path, omnic_report *rep) {
    FILE *in = fopen(path, "rb");
    omnic_reader r;
    omnic_decision d;
    omnic_status ret;

    if (in == NULL) {
        omnic_set_error("cannot open '%s': %s", path, strerror(errno));
        return OMNIC_E_SYSTEM;
    }
    omnic_reader_init(&r, in);
    ret = decide(in, path, &(omnic_options){OMNIC_PROFILE_BALANCED, NULL, 0, 0, 0, NULL}, &d);
    if (ret == OMNIC_OK) {
        rep->klass = d.klass;
        rep->entropy = d.entropy;
        if (d.pe != NULL) {
            snprintf(rep->codec, sizeof(rep->codec), "%s", d.pe->codec);
            rep->level = d.pe->level;
            snprintf(rep->reason, sizeof(rep->reason), "%s", d.pe->reason);
        }
    }
    free(d.seg_base); /* analyze 不试压（纯探测），中段只用于特征 */
    omnic_reader_fini(&r);
    fclose(in);
    return ret;
}
