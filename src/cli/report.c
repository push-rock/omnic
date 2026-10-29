#include "cli/report.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "core/util.h"

void omnic_print_report(const char *path, const omnic_report *rep, const omnic_options *o) {
    if (o->flags & OMNIC_F_QUIET) return;

    if (rep->skipped) {
        printf("%s -> skipped [%s | entropy %.2f bits/byte]\n", path, omnic_class_str(rep->klass),
               rep->entropy);
        if (o->flags & OMNIC_F_EXPLAIN) {
            printf("    %s\n", rep->reason);
        }
        return;
    }

    printf("%s: %s | %s", path, rep->codec, omnic_profile_str(o->profile));
    if (rep->klass != OMNIC_CLS_UNKNOWN || o->flags & OMNIC_F_EXPLAIN) {
        printf(" | %s", omnic_class_str(rep->klass));
    }
    if (rep->level != 0) printf(" | level %d", rep->level);
    printf("\n");

    if (rep->seconds > 0.0 && rep->in_bytes > 0) {
        printf("    %" PRIu64 " -> %" PRIu64 " bytes (%.2f%%), %.3fs, %.1f MB/s\n", rep->in_bytes,
               rep->out_bytes, rep->ratio * 100.0, rep->seconds, rep->mbps);
    }
    if ((o->flags & OMNIC_F_EXPLAIN) && rep->reason[0] != '\0') {
        printf("    %s\n", rep->reason);
    }
}
