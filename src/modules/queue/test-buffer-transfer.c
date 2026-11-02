/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "buffer-transfer.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spa/buffer/meta.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

static void test_copy(void)
{
	const uint8_t domain[SPA_ACQUISITION_DOMAIN_SIZE] = { 1 };
	const uint8_t grandmaster[SPA_EXPOSURE_PTP_CLOCK_ID_SIZE] = { 2 };
	uint8_t input_bytes[24], output_bytes[24];
	uint64_t input_properties[64], output_properties[64];
	uint32_t optical = SPA_META_OPTICAL_MIXED, trigger = SPA_META_TRIGGER_HIGH;
	struct spa_meta_header input_header = { .seq = 91, .pts = 1234 };
	struct spa_meta_header output_header = { 0 };
	struct spa_meta_busy input_busy = { .count = 9 };
	struct spa_meta_busy output_busy = { .count = 3 };
	struct spa_meta input_metas[3] = {
		{ SPA_META_Header, sizeof(input_header), &input_header },
		{ SPA_META_Properties, sizeof(input_properties), input_properties },
		{ SPA_META_Busy, sizeof(input_busy), &input_busy },
	};
	struct spa_meta output_metas[3] = {
		{ SPA_META_Busy, sizeof(output_busy), &output_busy },
		{ SPA_META_Header, sizeof(output_header), &output_header },
		{ SPA_META_Properties, sizeof(output_properties), output_properties },
	};
	struct spa_meta output_header_only = {
		SPA_META_Header, sizeof(output_header), &output_header,
	};
	struct spa_chunk input_chunks[2] = {
		{ .offset = 3, .size = 7, .stride = 11, .flags = 5 },
		{ .offset = 14, .size = 5, .stride = 13, .flags = 9 },
	};
	struct spa_chunk output_chunks[2] = { 0 };
	struct spa_data input_data[2] = {
		{ .type = SPA_DATA_MemPtr, .data = input_bytes,
				.maxsize = sizeof(input_bytes), .chunk = &input_chunks[0] },
		{ .type = SPA_DATA_MemPtr, .data = input_bytes,
				.maxsize = sizeof(input_bytes), .chunk = &input_chunks[1] },
	};
	struct spa_data output_data[2] = {
		{ .type = SPA_DATA_MemPtr, .data = output_bytes,
				.maxsize = sizeof(output_bytes), .chunk = &output_chunks[0] },
		{ .type = SPA_DATA_MemPtr, .data = output_bytes,
				.maxsize = sizeof(output_bytes), .chunk = &output_chunks[1] },
	};
	struct spa_buffer input = {
		.n_metas = 3, .metas = input_metas,
		.n_datas = 2, .datas = input_data,
	};
	struct spa_buffer output = {
		.n_metas = 3, .metas = output_metas,
		.n_datas = 2, .datas = output_data,
	};
	uint32_t i;

	CHECK(spa_meta_properties_init(&input_metas[1]));
	CHECK(spa_meta_properties_add(&input_metas[1], 1, 0, SPA_TYPE_Id, sizeof(optical), &optical));
	CHECK(spa_meta_properties_add(&input_metas[1], SPA_META_PROPERTY_triggerClassification,
			0, SPA_TYPE_Id, sizeof(trigger), &trigger));
	{
		uint32_t flags = SPA_POD_PROP_FLAG_MANDATORY, timebase = SPA_EXPOSURE_TIMEBASE_TAI, ptp_domain = 3;
		int64_t start = 123456;
		CHECK(spa_meta_properties_add(&input_metas[1], 3, flags, SPA_TYPE_Bytes, 16, domain));
		CHECK(spa_meta_properties_add_uint64(&input_metas[1], 4, flags, UINT64_MAX));
		CHECK(spa_meta_properties_add_uint64(&input_metas[1], 5, flags, UINT64_MAX));
		CHECK(spa_meta_properties_add_uint64(&input_metas[1], 6, flags, UINT64_MAX));
		CHECK(spa_meta_properties_add(&input_metas[1], 7, flags, SPA_TYPE_Long, 8, &start));
		CHECK(spa_meta_properties_add(&input_metas[1], 8, flags, SPA_TYPE_Id, 4, &timebase));
		CHECK(spa_meta_properties_add_uint64(&input_metas[1], 9, flags, UINT64_MAX));
		CHECK(spa_meta_properties_add(&input_metas[1], 10, flags, SPA_TYPE_Bytes, 8, grandmaster));
		CHECK(spa_meta_properties_add(&input_metas[1], 11, flags, SPA_TYPE_Id, 4, &ptp_domain));
	}
	CHECK(spa_meta_properties_add(&input_metas[1], 17, 0, SPA_TYPE_Bytes, 3, "abc"));
	for (i = 0; i < sizeof(input_bytes); i++)
		input_bytes[i] = (uint8_t)(i + 1u);
	memset(output_bytes, 0xa5, sizeof(output_bytes));
	CHECK(pwao_queue_buffer_transfer(&input, &output, true) == 0);
	CHECK(memcmp(&input_header, &output_header, sizeof(input_header)) == 0);
	CHECK(output_busy.count == 3);
	CHECK(memcmp(input_properties, output_properties, sizeof(input_properties)) == 0);
	CHECK(spa_meta_properties_find(&output_metas[2], SPA_META_PROPERTY_triggerClassification) != NULL);
	CHECK(spa_meta_properties_is_valid(&output_metas[2], NULL, 0));
	CHECK(memcmp(&input_chunks, &output_chunks, sizeof(input_chunks)) == 0);
	CHECK(memcmp(&input_bytes[3], &output_bytes[3], 7) == 0);
	CHECK(memcmp(&input_bytes[14], &output_bytes[14], 5) == 0);
	CHECK(output_bytes[2] == 0xa5 && output_bytes[10] == 0xa5 &&
			output_bytes[19] == 0xa5);
	/* Busy belongs to the output endpoint and need not be present in the
	 * producer's buffer layout. */
	input.n_metas = 2;
	output_busy.count = 5;
	CHECK(pwao_queue_buffer_transfer(&input, &output, true) == 0);
	CHECK(output_busy.count == 5);
	input_bytes[3] ^= 0xffu;
	CHECK(input_bytes[3] != output_bytes[3]);
	/* A consumer may negotiate only Header even when the producer also
	 * supplies Properties metadata. */
	output.n_metas = 1;
	output.metas = &output_header_only;
	output_header.seq = 0;
	CHECK(pwao_queue_buffer_transfer(&input, &output, true) == 0);
	CHECK(output_header.seq == input_header.seq);
	output_data[1].maxsize = 18;
	CHECK(pwao_queue_buffer_transfer(&input, &output, true) < 0);
}

static void test_lease(void)
{
	struct spa_meta_header input_header = { .seq = 7 };
	struct spa_meta_header output_header = { 0 };
	struct spa_meta input_meta = {
		SPA_META_Header, sizeof(input_header), &input_header,
	};
	struct spa_meta output_meta = {
		SPA_META_Header, sizeof(output_header), &output_header,
	};
	struct spa_chunk input_chunk = { .offset = 4, .size = 8, .stride = 8 };
	struct spa_chunk output_chunk = { 0 };
	struct spa_data input_data = { 0 }, output_data = { 0 };
	struct spa_buffer input = {
		.n_metas = 1, .metas = &input_meta,
		.n_datas = 1, .datas = &input_data,
	};
	struct spa_buffer output = {
		.n_metas = 1, .metas = &output_meta,
		.n_datas = 1, .datas = &output_data,
	};
	struct stat input_stat, output_stat;
	int owned_fd = -1;
	uint8_t *mapping;

	input_data.fd = memfd_create("pipewireao-queue-test", MFD_CLOEXEC);
	CHECK(input_data.fd >= 0);
	CHECK(ftruncate(input_data.fd, 64) == 0);
	mapping = mmap(NULL, 64, PROT_READ | PROT_WRITE, MAP_SHARED,
			input_data.fd, 0);
	CHECK(mapping != MAP_FAILED);
	input_data.type = SPA_DATA_MemFd;
	input_data.flags = SPA_DATA_FLAG_READWRITE | SPA_DATA_FLAG_MAPPABLE;
	input_data.data = mapping;
	input_data.maxsize = 64;
	input_data.chunk = &input_chunk;
	output_data.type = 1u << SPA_DATA_MemFd;
	output_data.chunk = &output_chunk;
	CHECK(pwao_queue_buffer_alias(&input, &output, &owned_fd, 1) == 0);
	CHECK(owned_fd >= 0 && owned_fd != input_data.fd);
	CHECK(output_data.fd == owned_fd && output_data.data == NULL);
	CHECK(output_data.maxsize == input_data.maxsize);
	CHECK(fstat(input_data.fd, &input_stat) == 0);
	CHECK(fstat(output_data.fd, &output_stat) == 0);
	CHECK(input_stat.st_dev == output_stat.st_dev &&
			input_stat.st_ino == output_stat.st_ino);
	CHECK(pwao_queue_buffer_transfer(&input, &output, false) == 0);
	CHECK(memcmp(&input_header, &output_header, sizeof(input_header)) == 0);
	CHECK(memcmp(&input_chunk, &output_chunk, sizeof(input_chunk)) == 0);
	mapping[4] = 0x5a;
	{
		uint8_t *output_mapping = mmap(NULL, 64, PROT_READ, MAP_SHARED,
				output_data.fd, 0);

		CHECK(output_mapping != MAP_FAILED);
		CHECK(output_mapping[4] == 0x5a);
		CHECK(munmap(output_mapping, 64) == 0);
	}
	pwao_queue_buffer_close_fds(&owned_fd, 1);
	CHECK(owned_fd == -1);
	CHECK(munmap(mapping, 64) == 0);
	CHECK(close(input_data.fd) == 0);

	input_data.type = SPA_DATA_MemPtr;
	input_data.fd = -1;
	output_data.type = 1u << SPA_DATA_MemFd;
	CHECK(pwao_queue_buffer_alias(&input, &output, &owned_fd, 1) < 0);
}

int main(void)
{
	test_copy();
	test_lease();
	return 0;
}
