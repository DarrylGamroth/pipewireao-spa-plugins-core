/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <string.h>

#include <spa/utils/defs.h>

#include "image-frame.h"

#define PAYLOAD_SIZE 64u

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	uint64_t properties[64];
	uint8_t payload[PAYLOAD_SIZE];
};

static void init_buffer(struct test_buffer *storage)
{
	memset(storage, 0, sizeof(*storage));
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.data = storage->payload;
	storage->data.maxsize = sizeof(storage->payload);
	storage->data.chunk = &storage->chunk;
	storage->metas[0] = (struct spa_meta) {
		.type = SPA_META_Header,
		.size = sizeof(storage->header),
		.data = &storage->header,
	};
	storage->metas[1] = (struct spa_meta) {
		.type = SPA_META_Properties,
		.size = sizeof(storage->properties),
		.data = storage->properties,
	};
	storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
	storage->buffer.metas = storage->metas;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
}

static void test_write(void)
{
	struct test_buffer storage;
	uint64_t source[64];
	struct spa_meta properties = { SPA_META_Properties, sizeof(source), source };
	const uint8_t domain[SPA_ACQUISITION_DOMAIN_SIZE] = { 1 };
	const uint32_t flags = SPA_POD_PROP_FLAG_MANDATORY;
	uint32_t optical = SPA_META_OPTICAL_MIXED, trigger = SPA_META_TRIGGER_HIGH;
	uint32_t timebase = SPA_EXPOSURE_TIMEBASE_MONOTONIC;
	int64_t start = 101, optional = 1234;
	struct spa_acquisition_identity identity;
	struct pwao_image_frame frame = {
		.data_index = 0,
		.header_flags = SPA_META_HEADER_FLAG_MARKER,
		.chunk_flags = SPA_CHUNK_FLAG_CORRUPTED,
		.offset = 4,
		.size = 32,
		.stride = 8,
		.header_offset = 12,
		.sequence = 3,
		.pts = 17,
		.properties = &properties,
	};

	init_buffer(&storage);
	spa_assert_se(spa_meta_properties_init(&properties));
	spa_assert_se(spa_meta_properties_add(&properties, 1, 0, SPA_TYPE_Id, sizeof(optical), &optical));
	spa_assert_se(spa_meta_properties_add(&properties, 2, 0, SPA_TYPE_Id, sizeof(trigger), &trigger));
	spa_assert_se(spa_meta_properties_add(&properties, 3, flags, SPA_TYPE_Bytes, sizeof(domain), domain));
	spa_assert_se(spa_meta_properties_add_uint64(&properties, 4, flags, UINT64_MAX));
	spa_assert_se(spa_meta_properties_add_uint64(&properties, 5, flags, UINT64_MAX));
	spa_assert_se(spa_meta_properties_add_uint64(&properties, 6, flags, UINT64_MAX));
	spa_assert_se(spa_meta_properties_add(&properties, 7, flags, SPA_TYPE_Long, sizeof(start), &start));
	spa_assert_se(spa_meta_properties_add(&properties, 8, flags, SPA_TYPE_Id, sizeof(timebase), &timebase));
	spa_assert_se(spa_meta_properties_add_uint64(&properties, 9, flags, UINT64_MAX));
	spa_assert_se(spa_meta_properties_add(&properties, 17, 0, SPA_TYPE_Long, sizeof(optional), &optional));
	memset(storage.properties, 0xa5, sizeof(storage.properties));
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER | PWAO_IMAGE_FRAME_REQUIRE_PROPERTIES) == 0);
	spa_assert_se(storage.chunk.offset == frame.offset);
	spa_assert_se(storage.chunk.size == frame.size);
	spa_assert_se(storage.chunk.stride == frame.stride);
	spa_assert_se((uint32_t)storage.chunk.flags == frame.chunk_flags);
	spa_assert_se(storage.header.flags == frame.header_flags);
	spa_assert_se(storage.header.offset == frame.header_offset);
	spa_assert_se(storage.header.seq == frame.sequence && storage.header.pts == frame.pts);
	spa_assert_se(memcmp(storage.properties, source, sizeof(source)) == 0);
	spa_assert_se(spa_meta_properties_get_acquisition_identity(&storage.metas[1], &identity));
	spa_assert_se(identity.generation == UINT64_MAX && identity.sequence == UINT64_MAX);
	/* An aliased source survives the same complete-object write. */
	frame.properties = &storage.metas[1];
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == 0);
	spa_assert_se(memcmp(storage.properties, source, sizeof(source)) == 0);
	/* Reusing the allocation without source facts clears every old field. */
	frame.properties = NULL;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_PROPERTIES) == 0);
	spa_assert_se(spa_meta_properties_is_valid(&storage.metas[1], NULL, 0));
	spa_assert_se(((struct spa_pod *)storage.properties)->size == sizeof(struct spa_pod_object_body));
}

static void test_properties_admission(void)
{
	struct test_buffer storage;
	uint64_t source[64];
	struct spa_meta properties = { SPA_META_Properties, sizeof(source), source };
	struct spa_meta_property_requirement required = { 25, SPA_TYPE_Long };
	const uint8_t domain[SPA_ACQUISITION_DOMAIN_SIZE] = { 1 };
	int64_t value = 42;
	struct pwao_image_frame frame = {
		.size = PAYLOAD_SIZE,
		.properties = &properties,
	};

	init_buffer(&storage);
	spa_assert_se(spa_meta_properties_init(&properties));
	spa_assert_se(spa_meta_properties_add(&properties, 3, SPA_POD_PROP_FLAG_MANDATORY,
			SPA_TYPE_Bytes, sizeof(domain), domain));
	/* Partial identity groups never reach publication. */
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	spa_assert_se(spa_meta_properties_init(&properties));
	spa_assert_se(spa_meta_properties_add(&properties, 25, SPA_POD_PROP_FLAG_MANDATORY,
			SPA_TYPE_Long, sizeof(value), &value));
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	frame.required_properties = &required;
	frame.n_required_properties = 1;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == 0);
	spa_assert_se(spa_meta_properties_is_valid(&storage.metas[1], &required, 1));
	required.type = SPA_TYPE_Id;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	required.type = SPA_TYPE_Long;
	required.key = 26;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	required.key = 25;
	storage.metas[1].size = SPA_META_PROPERTIES_MIN_SIZE;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -ENOSPC);
	storage.metas[1].size = sizeof(storage.properties);
	frame.properties = NULL;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	frame.n_required_properties = 0;
	storage.metas[1].type = 11;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -ENOTSUP);
	storage.metas[1].type = (12u << 16) | 1u;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -ENOTSUP);
}

static void test_validation(void)
{
	struct test_buffer storage;
	struct pwao_image_frame frame = {
		.data_index = 0,
		.size = PAYLOAD_SIZE,
		.pts = SPA_TIME_INVALID,
	};

	init_buffer(&storage);
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER) == 0);
	frame.size++;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -ENOSPC);
	frame.size--;
	frame.header_flags = UINT32_MAX;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame, 0) == -EINVAL);
	frame.header_flags = 0;
	storage.buffer.n_metas = 0;
	spa_assert_se(pwao_image_frame_write(&storage.buffer, &frame,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER) == -ENOTSUP);
}

int main(int argc SPA_UNUSED, char *argv[] SPA_UNUSED)
{
	test_write();
	test_properties_admission();
	test_validation();
	return 0;
}
