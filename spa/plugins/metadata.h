/* SPDX-License-Identifier: MIT */
#ifndef PWAO_METADATA_H
#define PWAO_METADATA_H
#include <spa/buffer/meta.h>
struct pwao_metadata {
    uint32_t count;
    struct spa_meta_requirement records[SPA_META_MAX_RECORDS];
};
/* NULL selects only required Header. Explicit arrays
 * replace additional records; Header remains required and may be stated once. */
int pwao_metadata_parse(struct pwao_metadata *metadata, const char *text);
#endif
