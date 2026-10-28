/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <stdio.h>
#include <stdlib.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>

#include "acquisition.h"

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
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
			SPA_PARAM_META_size, SPA_POD_Int(SPA_META_ACQUISITION_SIZE), 0);
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

static void pool_features(void)
{
	struct spa_meta_acquisition value = { 0 };
	struct spa_meta metas[] = {
		{ SPA_META_Acquisition, sizeof(value), &value },
		{ SPA_META_TYPE_FEATURES(SPA_META_Acquisition,
				SPA_META_FEATURE_ACQUISITION_VERSION_2), 0, NULL },
		{ SPA_META_TYPE_FEATURES(SPA_META_Acquisition,
				SPA_META_FEATURE_ACQUISITION_VERSION_2), 0, NULL },
	};
	struct spa_buffer buffer = { .n_metas = 2, .metas = metas };
	uint32_t features;
	const uint32_t invalid[] = {
		0, SPA_META_FEATURE_ACQUISITION_VERSION_1,
		SPA_META_FEATURE_ACQUISITION_VERSION_2 |
				SPA_META_FEATURE_ACQUISITION_VERSION_3,
		1u << 3,
	};

	/* The payload is not initialized yet; the negotiated marker is authoritative. */
	CHECK(acquisition_features(&buffer, &features) == 0);
	CHECK(features == SPA_META_FEATURE_ACQUISITION_VERSION_2);
	metas[1].type = SPA_META_TYPE_FEATURES(SPA_META_Acquisition,
			SPA_META_FEATURE_ACQUISITION_VERSION_3);
	CHECK(acquisition_features(&buffer, &features) == 0);
	CHECK(features == SPA_META_FEATURE_ACQUISITION_VERSION_3);
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(invalid); i++) {
		metas[1].type = SPA_META_TYPE_FEATURES(SPA_META_Acquisition, invalid[i]);
		features = 99;
		CHECK(acquisition_features(&buffer, &features) == -EINVAL);
		CHECK(features == 99);
	}
	metas[1].type = SPA_META_TYPE_FEATURES(SPA_META_Acquisition,
			SPA_META_FEATURE_ACQUISITION_VERSION_2);
	buffer.n_metas = 1;
	CHECK(acquisition_features(&buffer, &features) == -EINVAL); /* missing */
	buffer.n_metas = 3;
	CHECK(acquisition_features(&buffer, &features) == -EINVAL); /* duplicate */
	buffer.n_metas = 2;
	metas[1].size = 1;
	CHECK(acquisition_features(&buffer, &features) == -EINVAL);
}

int main(void)
{
	uint32_t v2 = SPA_META_FEATURE_ACQUISITION_VERSION_2;
	uint32_t v3 = SPA_META_FEATURE_ACQUISITION_VERSION_3;

	CHECK(SPA_META_FEATURE_ACQUISITION_CURRENT == v2);
	filter_features(v2, false, v3, -1);
	filter_features(v2 | v3, false, v3, -1); /* Int is exact, not a capability set. */
	filter_features(v2 | v3, true, v2, v2);
	filter_features(v2 | v3, true, v3, v3);
	filter_features(v3, false, v2, -1); /* A V3 pool must not promise V2 bytes. */
	filter_features(v3, false, v3, v3);
	filter_features(v2, false, v2, v2);
	pool_features();
	return 0;
}
