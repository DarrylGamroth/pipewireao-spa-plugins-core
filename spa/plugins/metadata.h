/* SPDX-License-Identifier: MIT */
#ifndef PWAO_METADATA_H
#define PWAO_METADATA_H
#include <spa/buffer/meta-json.h>
int pwao_metadata_parse(struct spa_meta_profile *metadata, const char *text);
#endif
