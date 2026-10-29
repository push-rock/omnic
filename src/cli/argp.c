#include "cli/argp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int profile_from_str(const char *s, omnic_profile *p) {
    if (strcmp(s, "fast_compress") == 0 || strcmp(s, "fc") == 0) {
        *p = OMNIC_PROFILE_FAST_COMPRESS;
    } else if (strcmp(s, "fast_decompress") == 0 || strcmp(s, "fd") == 0) {
        *p = OMNIC_PROFILE_FAST_DECOMPRESS;
    } else if (strcmp(s, "best_ratio") == 0 || strcmp(s, "br") == 0) {
        *p = OMNIC_PROFILE_BEST_RATIO;
    } else if (strcmp(s, "balanced") == 0 || strcmp(s, "bl") == 0) {
        *p = OMNIC_PROFILE_BALANCED;
    } else {
        return 1;
    }
    return 0;
}

static void push_file(omnic_args *a, const char *f) {
    if (a->nfiles == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 8;
        a->files = (const char **)realloc(a->files, sizeof(char *) * (size_t)a->cap);
    }
    a->files[a->nfiles++] = f;
}

static int need_value(int argc, char **argv, int *i, const char **out, const char *flag) {
    if (*i + 1 >= argc) {
        fprintf(stderr, "omnic: option '%s' requires a value\n", flag);
        return 1;
    }
    *out = argv[++(*i)];
    return 0;
}

static int parse_long_opt(omnic_args *a, int argc, char **argv, int *i) {
    const char *arg = argv[*i];
    const char *v;
    if (strcmp(arg, "--force") == 0) {
        a->opts.flags |= OMNIC_F_FORCE;
    } else if (strcmp(arg, "--keep") == 0) {
        a->opts.flags |= OMNIC_F_KEEP;
    } else if (strcmp(arg, "--quiet") == 0) {
        a->opts.flags |= OMNIC_F_QUIET;
    } else if (strcmp(arg, "--explain") == 0) {
        a->opts.flags |= OMNIC_F_EXPLAIN;
    } else if (strcmp(arg, "--profile") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        if (profile_from_str(v, &a->opts.profile) != 0) {
            fprintf(stderr, "omnic: invalid profile '%s' (fast_compress|fast_decompress|best_ratio|balanced)\n", v);
            return 1;
        }
    } else if (strcmp(arg, "--codec") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.codec = v;
    } else if (strcmp(arg, "--level") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.level = atoi(v);
        if (a->opts.level == 0 && strcmp(v, "0") != 0) {
            fprintf(stderr, "omnic: invalid level '%s'\n", v);
            return 1;
        }
    } else if (strcmp(arg, "--threads") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.threads = atoi(v);
        if (a->opts.threads < 0) {
            fprintf(stderr, "omnic: invalid threads '%s'\n", v);
            return 1;
        }
    } else if (strcmp(arg, "--output-dir") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.output_dir = v;
    } else if (strcmp(arg, "--version") == 0) {
        a->cmd = "version";
    } else if (strcmp(arg, "--help") == 0) {
        a->cmd = "help";
    } else {
        fprintf(stderr, "omnic: unknown option '%s'\n", arg);
        return 1;
    }
    return 0;
}

static int parse_short_opt(omnic_args *a, int argc, char **argv, int *i) {
    const char *arg = argv[*i];
    const char *v;
    if (strcmp(arg, "-f") == 0) {
        a->opts.flags |= OMNIC_F_FORCE;
    } else if (strcmp(arg, "-k") == 0) {
        a->opts.flags |= OMNIC_F_KEEP;
    } else if (strcmp(arg, "-q") == 0) {
        a->opts.flags |= OMNIC_F_QUIET;
    } else if (strcmp(arg, "-x") == 0) {
        a->opts.flags |= OMNIC_F_EXPLAIN;
    } else if (strcmp(arg, "-p") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        if (profile_from_str(v, &a->opts.profile) != 0) {
            fprintf(stderr, "omnic: invalid profile '%s'\n", v);
            return 1;
        }
    } else if (strcmp(arg, "-c") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.codec = v;
    } else if (strcmp(arg, "-l") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.level = atoi(v);
    } else if (strcmp(arg, "-T") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.threads = atoi(v);
    } else if (strcmp(arg, "-o") == 0) {
        if (need_value(argc, argv, i, &v, arg) != 0) return 1;
        a->opts.output_dir = v;
    } else if (strcmp(arg, "-h") == 0) {
        a->cmd = "help";
    } else if (strcmp(arg, "-V") == 0) {
        a->cmd = "version";
    } else {
        fprintf(stderr, "omnic: unknown option '%s'\n", arg);
        return 1;
    }
    return 0;
}

int omnic_parse_args(int argc, char **argv, omnic_args *a) {
    memset(a, 0, sizeof(*a));
    a->opts.profile = OMNIC_PROFILE_BALANCED;

    if (argc < 2) {
        a->cmd = "help";
        return 0;
    }
    const char *first = argv[1];
    if (strcmp(first, "c") == 0 || strcmp(first, "compress") == 0) {
        a->cmd = "c";
    } else if (strcmp(first, "d") == 0 || strcmp(first, "decompress") == 0) {
        a->cmd = "d";
    } else if (strcmp(first, "analyze") == 0) {
        a->cmd = "analyze";
    } else if (strcmp(first, "list") == 0) {
        a->cmd = "list";
    } else if (strcmp(first, "version") == 0 || strcmp(first, "--version") == 0 ||
               strcmp(first, "-V") == 0) {
        a->cmd = "version";
    } else if (strcmp(first, "help") == 0 || strcmp(first, "--help") == 0 ||
               strcmp(first, "-h") == 0) {
        a->cmd = "help";
    } else {
        fprintf(stderr, "omnic: unknown command '%s'\n", first);
        return 1;
    }

    for (int i = 2; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] == '-' && argv[i][2] != '\0') {
            if (parse_long_opt(a, argc, argv, &i) != 0) return 1;
        } else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            if (parse_short_opt(a, argc, argv, &i) != 0) return 1;
        } else {
            push_file(a, argv[i]);
        }
    }

    if ((strcmp(a->cmd, "c") == 0 || strcmp(a->cmd, "d") == 0 || strcmp(a->cmd, "analyze") == 0) &&
        a->nfiles == 0) {
        fprintf(stderr, "omnic: '%s' requires at least one file\n", a->cmd);
        return 1;
    }
    return 0;
}
