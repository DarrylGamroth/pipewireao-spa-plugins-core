/* SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <string.h>
#include <spa/utils/defs.h>
#include "image-frame.h"

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[3];
	struct spa_meta_header header;
	struct spa_meta_acquisition_identity identity;
	uint64_t opaque;
	uint8_t payload[64];
};
static void init_buffer(struct test_buffer *s)
{
	memset(s, 0, sizeof(*s));
	s->data = (struct spa_data) { .type = SPA_DATA_MemPtr, .data = s->payload,
		.maxsize = sizeof(s->payload), .chunk = &s->chunk };
	s->metas[0] = (struct spa_meta) { SPA_META_Header, sizeof(s->header), &s->header };
	s->metas[1] = (struct spa_meta) { SPA_META_AcquisitionIdentity, sizeof(s->identity), &s->identity };
	s->metas[2] = (struct spa_meta) { 0x3fe, sizeof(s->opaque), &s->opaque };
	s->buffer = (struct spa_buffer) { .n_metas = 3, .metas = s->metas,
		.n_datas = 1, .datas = &s->data };
}
static void test_preserve_and_reuse(void)
{
	struct test_buffer output;
	struct spa_meta_acquisition_identity identity = {
		.flags = SPA_META_RECORD_VALID, .domain = { 1 },
		.generation = UINT64_MAX, .sequence = UINT64_MAX - 1 };
	uint64_t opaque = UINT64_MAX;
	struct spa_meta source[] = {
		{ SPA_META_AcquisitionIdentity, sizeof(identity), &identity },
		{ 0x3fe, sizeof(opaque), &opaque },
	};
	struct pwao_image_frame frame = { .size = 32, .offset = 4,
		.stride = 8, .sequence = 123, .pts = 17, .metas = source, .n_metas = 2 };
	init_buffer(&output);
	spa_assert_se(pwao_image_frame_write(&output.buffer, &frame, PWAO_IMAGE_FRAME_REQUIRE_HEADER) == 0);
	spa_assert_se(output.header.seq == 123 && output.header.pts == 17);
	spa_assert_se(output.identity.sequence == UINT64_MAX - 1 && output.identity.generation == UINT64_MAX);
	spa_assert_se(output.opaque == UINT64_MAX && output.chunk.size == 32);
	/* The source remains unchanged, including an optional opaque payload. */
	spa_assert_se(identity.sequence == UINT64_MAX - 1 && opaque == UINT64_MAX);
	frame.metas = NULL; frame.n_metas = 0;
	spa_assert_se(pwao_image_frame_write(&output.buffer, &frame, 0) == 0);
	spa_assert_se(output.identity.flags == 0 && output.identity.sequence == 0 && output.opaque == 0);
}
static void test_rejection_before_publication(void)
{
	struct test_buffer output;
	struct spa_meta_acquisition_identity identity = { 0 };
	struct spa_meta source = { SPA_META_AcquisitionIdentity, sizeof(identity), &identity };
	struct spa_meta_requirement requirement = { SPA_META_AcquisitionIdentity, sizeof(identity), SPA_META_REQUIREMENT_REQUIRED, 0 };
	struct pwao_image_frame frame = { .size = 32, .metas = &source, .n_metas = 1,
		.required_records = &requirement, .n_required_records = 1 };
	init_buffer(&output); output.header.seq = 91;
	spa_assert_se(pwao_image_frame_write(&output.buffer, &frame, 0) == -EINVAL);
	spa_assert_se(output.header.seq == 91 && output.chunk.size == 0);
	identity = (struct spa_meta_acquisition_identity) { .flags = SPA_META_RECORD_VALID, .domain = { 1 } };
	output.buffer.n_metas = 1;
	spa_assert_se(pwao_image_frame_write(&output.buffer, &frame, 0) == -ENOSPC);
	spa_assert_se(output.header.seq == 91 && output.chunk.size == 0);
	output.buffer.n_metas = 3; frame.size = 65;
	spa_assert_se(pwao_image_frame_write(&output.buffer, &frame, 0) == -ENOSPC);
	frame.size = 32; frame.header_flags = UINT32_MAX;
	spa_assert_se(pwao_image_frame_write(&output.buffer, &frame, 0) == -EINVAL);
}
int main(void) { test_preserve_and_reuse(); test_rejection_before_publication(); return 0; }
