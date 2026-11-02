/* SPDX-License-Identifier: MIT */
#include "../../../spa/plugins/metadata.h"
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) abort(); } while (0)
#include <stdio.h>
#include <string.h>

int main(void)
{
    struct pwao_metadata metadata;
    CHECK(pwao_metadata_parse(&metadata, NULL) == 0 && metadata.count == 1);
    CHECK(pwao_metadata_parse(&metadata, "[]") == 0 && metadata.count == 1);
    CHECK(pwao_metadata_parse(&metadata, "[{type=1022 size=8 required=true}]") == 0);
    CHECK(metadata.count == 2 && metadata.records[1].type == 1022 && metadata.records[1].flags == SPA_META_REQUIREMENT_REQUIRED);
    CHECK(pwao_metadata_parse(&metadata, "[{type=1 size=32 required=true}]") == 0 && metadata.count == 1);
    const char *invalid[] = {
        "[", "{}", "[] true", "[1]", "[{type=1022 size=8}]", "[{type=1022 size=8 required=1}]",
        "[{type=1022 size=8 required=true unknown=0}]", "[{type=1022 type=1023 size=8 required=true}]",
        "[{type=1022 size=8 required=true},{type=1022 size=8 required=false}]",
        "[{type=1 size=32 required=false}]", "[{type=1 size=40 required=true}]",
        "[{type=1 size=32 required=true},{type=1 size=32 required=true}]",
        "[{type=768 size=96 required=true}]", "[{type=1022 size=4294967296 required=true}]",
        "[{type=1022 size=-1 required=true}]", "[{type=13 size=512 required=true}]",
        "[{type=7 size=8 required=true}]", "[{type=9 size=24 required=true}]",
        "[{type=65536 size=0 required=false}]",
    };
    for (size_t i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        int result = pwao_metadata_parse(&metadata, invalid[i]);
        if (result >= 0) fprintf(stderr, "accepted invalid metadata: %s\n", invalid[i]);
        CHECK(result < 0);
    }
    CHECK(pwao_metadata_parse(&metadata, "[{type=1022 size=4064 required=true}]") == 0);
    CHECK(pwao_metadata_parse(&metadata, "[{type=1022 size=4065 required=true}]") < 0);
    char text[8192]; size_t length = 1; text[0] = '[';
    for (unsigned i = 0; i < 64; i++) {
        length += snprintf(text + length, sizeof(text) - length, "{type=%u size=8 required=false}", 0x380 + i);
        text[length] = ']'; text[length + 1] = 0;
        CHECK((pwao_metadata_parse(&metadata, text) == 0) == (i < 63));
    }
    return 0;
}
