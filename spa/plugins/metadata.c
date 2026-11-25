/* SPDX-License-Identifier: MIT */
#include "metadata.h"

int pwao_metadata_parse(struct spa_meta_profile *metadata, const char *text)
{
    return spa_meta_profile_parse(metadata, text);
}
