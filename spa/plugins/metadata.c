/* SPDX-License-Identifier: MIT */
#include "metadata.h"
#include <errno.h>
#include <string.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

static int unsigned_value(const char *value, int length, uint32_t *result)
{
    char text[64];
    if (length <= 0 || length >= (int)sizeof(text) || value[0] == '-') return -EINVAL;
    memcpy(text, value, length); text[length] = 0;
    return spa_atou32(text, result, 0) ? 0 : -EINVAL;
}

static int validate_layout(const struct pwao_metadata *metadata)
{
    uint32_t offset = 0;
    for (uint32_t i = 0; i < metadata->count; i++) {
        const struct spa_meta_requirement *r = &metadata->records[i];
        if (!spa_meta_record_layout_validate(r->type, r->size, SPA_META_MAX_BYTES) ||
            spa_meta_type_is_transport_owned(r->type)) return -EINVAL;
        uint32_t aligned = SPA_ROUND_UP_N(r->size, 8);
        if (aligned > SPA_META_MAX_BYTES - offset) return -ENOSPC;
        offset += aligned;
    }
    return 0;
}

int pwao_metadata_parse(struct pwao_metadata *metadata, const char *text)
{
    struct spa_json root, array, object;
    const char *value;
    bool explicit_header = false;
    int res;
    *metadata = (struct pwao_metadata) { .count = 1,
        .records = { { SPA_META_Header, sizeof(struct spa_meta_header), SPA_META_REQUIREMENT_REQUIRED, 0 } } };
    if (text == NULL) return validate_layout(metadata);
    if (strlen(text) > SPA_META_MAX_RECORDS * 256u) return -ENOSPC;
    spa_json_init(&root, text, strlen(text));
    if (spa_json_enter_array(&root, &array) <= 0) return -EINVAL;
    while ((res = spa_json_enter_object(&array, &object)) > 0) {
        struct spa_meta_requirement record = { 0 };
        char key[32];
        uint32_t fields = 0;
        int length;
        while ((length = spa_json_get_string(&object, key, sizeof(key))) > 0) {
            length = spa_json_next(&object, &value);
            if (length <= 0) return -EINVAL;
            uint32_t bit;
            if (spa_streq(key, "type")) {
                bit = 1;
                if (unsigned_value(value, length, &record.type) < 0) return -EINVAL;
            } else if (spa_streq(key, "size")) {
                bit = 2;
                if (unsigned_value(value, length, &record.size) < 0) return -EINVAL;
            } else if (spa_streq(key, "required")) {
                bool required;
                bit = 4;
                if (spa_json_parse_bool(value, length, &required) <= 0) return -EINVAL;
                record.flags = required ? SPA_META_REQUIREMENT_REQUIRED : 0;
            } else return -EINVAL;
            if (fields & bit) return -EEXIST;
            fields |= bit;
        }
        if (length < 0 || fields != 7) return -EINVAL;
        if (record.type == SPA_META_Header) {
            if (explicit_header) return -EEXIST;
            explicit_header = true;
            if (record.size != sizeof(struct spa_meta_header) || record.flags != SPA_META_REQUIREMENT_REQUIRED) return -EINVAL;
            continue;
        }
        if (metadata->count == SPA_META_MAX_RECORDS) return -ENOSPC;
        for (uint32_t i = 0; i < metadata->count; i++)
            if (metadata->records[i].type == record.type) return -EEXIST;
        metadata->records[metadata->count++] = record;
    }
    if (res < 0 || spa_json_next(&root, &value) != 0) return -EINVAL;
    return validate_layout(metadata);
}
