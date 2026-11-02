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

static struct spa_pod *meta(struct spa_pod_builder *builder, uint32_t size)
{
	return spa_pod_builder_add_object(builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
		SPA_PARAM_META_type, SPA_POD_Id(SPA_META_AcquisitionIdentity),
		SPA_PARAM_META_size, SPA_POD_Int(size),
		SPA_PARAM_META_required, SPA_POD_Bool(true));
}
static void filter_layout(uint32_t peer_size, bool succeeds)
{
	uint64_t a[64], b[64], c[64];
	struct spa_pod_builder ours = SPA_POD_BUILDER_INIT(a, sizeof(a));
	struct spa_pod_builder peer = SPA_POD_BUILDER_INIT(b, sizeof(b));
	struct spa_pod_builder result_builder = SPA_POD_BUILDER_INIT(c, sizeof(c));
	struct spa_pod *result;
	int res = spa_pod_filter(&result_builder, &result, meta(&ours, 40), meta(&peer, peer_size));
	CHECK((res >= 0) == succeeds);
}
int main(void) { filter_layout(40, true); filter_layout(96, false); return 0; }
