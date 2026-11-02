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
	uint8_t input_bytes[24], output_bytes[24];
	uint64_t input_opaque[64] = { 0x1234, UINT64_MAX }, output_opaque[64];
	struct spa_meta_header input_header = { .seq = 91, .pts = 1234 };
	struct spa_meta_header output_header = { 0 };
	struct spa_meta_busy input_busy = { .count = 9 };
	struct spa_meta_busy output_busy = { .count = 3 };
    struct spa_meta_sync_timeline input_sync = { .acquire_point = 11, .release_point = 12 };
    struct spa_meta_sync_timeline output_sync = { .acquire_point = 77, .release_point = 88 };
	struct spa_meta input_metas[4] = {
		{ SPA_META_Header, sizeof(input_header), &input_header },
		{ 0x3fe, sizeof(input_opaque), input_opaque },
		{ SPA_META_Busy, sizeof(input_busy), &input_busy },
        { SPA_META_SyncTimeline, sizeof(input_sync), &input_sync },
	};
	struct spa_meta output_metas[4] = {
		{ SPA_META_Busy, sizeof(output_busy), &output_busy },
		{ SPA_META_Header, sizeof(output_header), &output_header },
		{ 0x3fe, sizeof(output_opaque), output_opaque },
        { SPA_META_SyncTimeline, sizeof(output_sync), &output_sync },
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
		.n_metas = 4, .metas = input_metas,
		.n_datas = 2, .datas = input_data,
	};
	struct spa_buffer output = {
		.n_metas = 4, .metas = output_metas,
		.n_datas = 2, .datas = output_data,
	};
	uint32_t i;

	for (i = 0; i < sizeof(input_bytes); i++)
		input_bytes[i] = (uint8_t)(i + 1u);
	memset(output_bytes, 0xa5, sizeof(output_bytes));
	CHECK(pwao_queue_buffer_transfer(&input, &output, true) == 0);
	CHECK(memcmp(&input_header, &output_header, sizeof(input_header)) == 0);
	CHECK(output_busy.count == 3);
    CHECK(output_sync.acquire_point == 77 && output_sync.release_point == 88);
	CHECK(memcmp(input_opaque, output_opaque, sizeof(input_opaque)) == 0);
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
	/* A preserve port cannot drop an opaque record for a Header-only peer. */
	output.n_metas = 1;
	output.metas = &output_header_only;
	output_header.seq = 0;
	CHECK(pwao_queue_buffer_transfer(&input, &output, true) == -ENOSPC);
	CHECK(output_header.seq == 0);
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

static void test_full_record_capacity(void)
{
    uint64_t input_values[64][4] = { 0 }, output_values[64][4] = { 0 };
    struct spa_meta input_metas[64], output_metas[64];
    uint8_t input_byte = 7, output_byte = 0;
    struct spa_chunk input_chunk = { .size = 1 }, output_chunk = { 0 };
    struct spa_data input_data = { .type = SPA_DATA_MemPtr, .data = &input_byte, .maxsize = 1, .chunk = &input_chunk };
    struct spa_data output_data = { .type = SPA_DATA_MemPtr, .data = &output_byte, .maxsize = 1, .chunk = &output_chunk };
    struct spa_buffer input = { .n_metas = 64, .metas = input_metas, .n_datas = 1, .datas = &input_data };
    struct spa_buffer output = { .n_metas = 64, .metas = output_metas, .n_datas = 1, .datas = &output_data };
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t type = i == 0 ? SPA_META_Header : 0x380 + i;
        uint32_t size = i == 0 ? sizeof(struct spa_meta_header) : 8;
        input_values[i][0] = i;
        input_metas[i] = (struct spa_meta) { type, size, input_values[i] };
        output_metas[i] = (struct spa_meta) { type, size, output_values[i] };
    }
    CHECK(pwao_queue_buffer_transfer(&input, &output, true) == 0);
    CHECK(output_byte == 7);
    for (uint32_t i = 0; i < 64; i++) CHECK(output_values[i][0] == i);
    input.n_metas = 65;
    CHECK(pwao_queue_buffer_transfer(&input, &output, true) == -EINVAL);
}

int main(void)
{
	test_copy();
    test_full_record_capacity();
	test_lease();
	return 0;
}
