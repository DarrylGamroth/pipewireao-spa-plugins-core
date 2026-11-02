/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>

#include <spa/buffer/meta.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

static struct spa_pod *meta(struct spa_pod_builder *builder,
		uint32_t features, bool choice)
{
	struct spa_pod_frame object, flags;

	spa_pod_builder_push_object(builder, &object,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
	spa_pod_builder_add(builder,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Properties),
			SPA_PARAM_META_size, SPA_POD_Int(512), 0);
	spa_pod_builder_prop(builder, SPA_PARAM_META_features,
			SPA_POD_PROP_FLAG_MANDATORY);
	if (choice) {
		spa_pod_builder_push_choice(builder, &flags, SPA_CHOICE_Flags, 0);
		spa_pod_builder_int(builder, features);
		spa_pod_builder_pop(builder, &flags);
	} else
		spa_pod_builder_int(builder, features);
	return spa_pod_builder_pop(builder, &object);
}

static void filter_features(uint32_t capture, bool choice,
		uint32_t peer, int expected)
{
	uint8_t a[256], b[256], c[256];
	struct spa_pod_builder capture_builder = SPA_POD_BUILDER_INIT(a, sizeof(a));
	struct spa_pod_builder peer_builder = SPA_POD_BUILDER_INIT(b, sizeof(b));
	struct spa_pod_builder result_builder = SPA_POD_BUILDER_INIT(c, sizeof(c));
	struct spa_pod *result;
	int features = 0;
	int res = spa_pod_filter(&result_builder, &result,
			meta(&capture_builder, capture, choice),
			meta(&peer_builder, peer, false));

	if (expected < 0) {
		CHECK(res < 0);
		return;
	}
	CHECK(res >= 0);
	spa_pod_fixate(result);
	CHECK(spa_pod_parse_object(result, SPA_TYPE_OBJECT_ParamMeta, NULL,
			SPA_PARAM_META_features, SPA_POD_Int(&features)) >= 0);
	CHECK(features == expected);
}

int main(void)
{
	uint32_t supported = SPA_META_FEATURE_PROPERTIES_VERSION_1;
	filter_features(supported, false, supported, supported);
	filter_features(supported, false, 2, -1);
	filter_features(supported, false, 0, -1);
	return 0;
}
