#include "policy/policy.h"

#include <string.h>

extern const omnic_policy_entry omnic_policy_entries[];
extern const size_t omnic_policy_entries_n;

const omnic_policy_entry *omnic_policy_lookup(omnic_class c, omnic_profile p) {
    /* 28 项线性查，远低于 1ms 预算（SYS-CMP-002） */
    for (size_t i = 0; i < omnic_policy_entries_n; i++) {
        if (omnic_policy_entries[i].klass == (uint8_t)c && omnic_policy_entries[i].profile == (uint8_t)p) {
            return &omnic_policy_entries[i];
        }
    }
    return NULL;
}

bool omnic_policy_is_skip(const omnic_policy_entry *e) {
    return e != NULL && strcmp(e->codec, OMNIC_SKIP_CODEC) == 0;
}

size_t omnic_policy_entry_count(void) { return omnic_policy_entries_n; }

const omnic_policy_entry *omnic_policy_entry_at(size_t i) {
    return i < omnic_policy_entries_n ? &omnic_policy_entries[i] : NULL;
}
