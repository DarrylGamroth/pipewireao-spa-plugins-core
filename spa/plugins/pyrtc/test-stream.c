/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spa/support/plugin.h>

#include "stream.h"

static void make_name(char *name, size_t size, const char *suffix)
{
	spa_assert_se(snprintf(name, size, "pwao_pyrtc_%ld_%s",
			(long)getpid(), suffix) < (int)size);
}

static void test_dtype_mapping(void)
{
	static const enum spa_element_type element_types[] = {
		SPA_ELEMENT_TYPE_I8,
		SPA_ELEMENT_TYPE_I16_LE,
		SPA_ELEMENT_TYPE_I32_LE,
		SPA_ELEMENT_TYPE_I64_LE,
		SPA_ELEMENT_TYPE_U8,
		SPA_ELEMENT_TYPE_U16_LE,
		SPA_ELEMENT_TYPE_U32_LE,
		SPA_ELEMENT_TYPE_U64_LE,
		SPA_ELEMENT_TYPE_F16_LE,
		SPA_ELEMENT_TYPE_F32_LE,
		SPA_ELEMENT_TYPE_F64_LE,
		SPA_ELEMENT_TYPE_COMPLEX_F32_LE,
		SPA_ELEMENT_TYPE_COMPLEX_F64_LE,
		SPA_ELEMENT_TYPE_BOOL8,
	};
	const uint32_t shape[PYRTC_MAX_DIMENSIONS + 1u] = { 1, 2, 3, 4, 5, 6 };
	struct pyrtc_format format;
	struct pyrtc_stream *stream = NULL;
	uint32_t i;

	for (i = 0; i < SPA_N_ELEMENTS(element_types); i++) {
		spa_assert_se(pyrtc_format_from_element(element_types[i], 1, shape,
				&format) == 0);
		spa_assert_se(format.dtype_index == i);
	}
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_F32_LE,
			PYRTC_MAX_DIMENSIONS, shape, &format) == 0);
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_F32_LE,
			PYRTC_MAX_DIMENSIONS + 1u, shape, &format) == -EINVAL);
	spa_assert_se(pyrtc_stream_create("/invalid", &format, &stream) == -EINVAL);
	const uint32_t empty[PYRTC_MAX_DIMENSIONS] = { 0 };
	const uint32_t oversized[PYRTC_MAX_DIMENSIONS] = { INT32_MAX, INT32_MAX, INT32_MAX };
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_F32_LE, 1, empty, &format) == -EINVAL);
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_F32_LE, 3, oversized, &format) == -EOVERFLOW);
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_UNKNOWN, 1, shape, &format) == -ENOTSUP);
}

static void test_roundtrip(void)
{
	const uint32_t shape[PYRTC_MAX_DIMENSIONS] = { 3, 4 };
	const uint16_t expected[] = {
		1, 2, 3, 4,
		5, 6, 7, 8,
		9, 10, 11, 12,
	};
	uint16_t observed[SPA_N_ELEMENTS(expected)] = { 0 };
	struct pyrtc_format format, opened;
	struct pyrtc_stream *writer = NULL, *reader = NULL;
	char name[128], object_name[130];
	double *metadata;
	uint64_t sequence = 0;
	bool available = false;
	int fd;

	make_name(name, sizeof(name), "roundtrip");
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_U16_LE, 2,
			shape, &format) == 0);
	spa_assert_se(format.bytes == sizeof(expected));
	spa_assert_se(format.stride == 4u * sizeof(uint16_t));
	spa_assert_se(pyrtc_stream_create(name, &format, &writer) == 0);
	spa_assert_se(pyrtc_stream_create(name, &format, &reader) == -EEXIST);

	spa_assert_se(snprintf(object_name, sizeof(object_name), "/%s_meta", name) <
			(int)sizeof(object_name));
	fd = shm_open(object_name, O_RDONLY, 0);
	spa_assert_se(fd >= 0);
	metadata = mmap(NULL, PYRTC_METADATA_VALUES * sizeof(double), PROT_READ,
			MAP_SHARED, fd, 0);
	spa_assert_se(metadata != MAP_FAILED);
	spa_assert_se(metadata[0] == 0.0 && metadata[1] == 0.0 &&
			metadata[2] == sizeof(expected) && metadata[3] == 5.0 &&
			metadata[4] == 3.0 && metadata[5] == 4.0 && metadata[6] == 0.0);
	spa_assert_se(munmap(metadata, PYRTC_METADATA_VALUES * sizeof(double)) == 0);
	spa_assert_se(close(fd) == 0);

	spa_assert_se(pyrtc_stream_open(name, true, &reader, &opened) == 0);
	spa_assert_se(pyrtc_stream_has_update(writer, false, &available) == -EINVAL);
	spa_assert_se(pyrtc_stream_has_update(reader, true, &available) == 0 && available);
	spa_assert_se(pyrtc_stream_read(reader, observed, sizeof(observed) - 1, &sequence) == -EINVAL);
	spa_assert_se(pyrtc_stream_write(writer, expected, sizeof(expected) - 1) == -EINVAL);
	spa_assert_se(pyrtc_stream_write(reader, expected, sizeof(expected)) == -EINVAL);
	spa_assert_se(opened.rank == 2 && opened.shape[0] == 3 &&
			opened.shape[1] == 4 &&
			opened.element_type == SPA_ELEMENT_TYPE_U16_LE);
	spa_assert_se(pyrtc_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(!available);
	spa_assert_se(pyrtc_stream_write(writer, expected, sizeof(expected)) == 0);
	spa_assert_se(pyrtc_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(available);
	spa_assert_se(pyrtc_stream_read(reader, observed, sizeof(observed),
			&sequence) == 0);
	spa_assert_se(sequence == 1 &&
			memcmp(observed, expected, sizeof(expected)) == 0);
	spa_assert_se(pyrtc_stream_has_update(reader, false, &available) == 0);
	spa_assert_se(!available);
	spa_assert_se(pyrtc_stream_write(writer, expected, sizeof(expected)) == 0);
	spa_assert_se(pyrtc_stream_write(writer, expected, sizeof(expected)) == 0);
	spa_assert_se(pyrtc_stream_read(reader, observed, sizeof(observed),
			&sequence) == 0);
	spa_assert_se(sequence == 3);
	pyrtc_stream_close(writer);
	spa_assert_se(shm_open(object_name, O_RDONLY, 0) < 0 && errno == ENOENT);
	pyrtc_stream_close(reader);
}

static void test_raw_pyrtc_stream(void)
{
	const uint32_t shape[] = { 2, 3, 4 };
	const size_t bytes = 24;
	struct pyrtc_format opened;
	struct pyrtc_stream *reader = NULL;
	char name[128], data_name[130], metadata_name[136];
	double metadata[PYRTC_METADATA_VALUES] = { 7.0, 1234.5, bytes, 13.0,
		2.0, 3.0, 4.0 };
	void *mapping;
	int data_fd, metadata_fd;

	make_name(name, sizeof(name), "raw");
	spa_assert_se(snprintf(data_name, sizeof(data_name), "/%s", name) <
			(int)sizeof(data_name));
	spa_assert_se(snprintf(metadata_name, sizeof(metadata_name), "/%s_meta", name) <
			(int)sizeof(metadata_name));
	data_fd = shm_open(data_name, O_CREAT | O_EXCL | O_RDWR, 0600);
	metadata_fd = shm_open(metadata_name, O_CREAT | O_EXCL | O_RDWR, 0600);
	spa_assert_se(data_fd >= 0 && metadata_fd >= 0);
	spa_assert_se(ftruncate(data_fd, (off_t)bytes) == 0);
	spa_assert_se(ftruncate(metadata_fd,
			(off_t)(PYRTC_METADATA_VALUES * sizeof(double))) == 0);
	mapping = mmap(NULL, sizeof(metadata), PROT_READ | PROT_WRITE, MAP_SHARED,
			metadata_fd, 0);
	spa_assert_se(mapping != MAP_FAILED);
	memcpy(mapping, metadata, sizeof(metadata));
	static const struct { size_t field; double value; int status; } cases[] = {
		{ 0, -1.0, -EPROTO },
		{ 0, 1.5, -EPROTO },
		{ 2, 23.0, -EPROTO },
		{ 3, NAN, -EPROTO },
		{ 3, 14.0, -ENOTSUP },
		{ 4, 0.0, -EPROTO },
		{ 5, 2.5, -EPROTO },
	};
	for (size_t i = 0; i < SPA_N_ELEMENTS(cases); i++) {
		double *values = mapping;
		values[cases[i].field] = cases[i].value;
		spa_assert_se(pyrtc_stream_open(name, true, &reader, &opened) == cases[i].status);
		spa_assert_se(reader == NULL);
		values[cases[i].field] = metadata[cases[i].field];
	}
	spa_assert_se(ftruncate(data_fd, (off_t)bytes - 1) == 0);
	spa_assert_se(pyrtc_stream_open(name, true, &reader, &opened) == -EPROTO);
	spa_assert_se(ftruncate(data_fd, (off_t)bytes) == 0);
	spa_assert_se(pyrtc_stream_open(name, true, &reader, &opened) == 0);
	spa_assert_se(opened.element_type == SPA_ELEMENT_TYPE_BOOL8 &&
			opened.rank == SPA_N_ELEMENTS(shape) &&
			memcmp(opened.shape, shape, sizeof(shape)) == 0 &&
			opened.bytes == bytes);
	uint8_t payload[24] = { 0 };
	uint64_t sequence = 0;
	double *values = mapping;
	values[0] = NAN;
	spa_assert_se(pyrtc_stream_read(reader, payload, sizeof(payload), &sequence) == -EPROTO);
	values[0] = metadata[0];
	spa_assert_se(pyrtc_stream_read(reader, payload, sizeof(payload), &sequence) == 0);
	spa_assert_se(sequence == 7);
	pyrtc_stream_close(reader);
	spa_assert_se(munmap(mapping, sizeof(metadata)) == 0);
	spa_assert_se(close(metadata_fd) == 0 && close(data_fd) == 0);
	spa_assert_se(shm_unlink(metadata_name) == 0);
	spa_assert_se(shm_unlink(data_name) == 0);
}

static void test_metadata_collision(void)
{
	const uint32_t shape[PYRTC_MAX_DIMENSIONS] = { 4 };
	struct pyrtc_format format;
	struct pyrtc_stream *writer = NULL;
	char name[128], data_name[130], metadata_name[136];
	int fd, probe;

	make_name(name, sizeof(name), "collision");
	spa_assert_se(snprintf(data_name, sizeof(data_name), "/%s", name) <
			(int)sizeof(data_name));
	spa_assert_se(snprintf(metadata_name, sizeof(metadata_name), "/%s_meta", name) <
			(int)sizeof(metadata_name));
	fd = shm_open(metadata_name, O_CREAT | O_EXCL | O_RDWR, 0600);
	spa_assert_se(fd >= 0);
	spa_assert_se(ftruncate(fd,
			(off_t)(PYRTC_METADATA_VALUES * sizeof(double))) == 0);
	spa_assert_se(pyrtc_format_from_element(SPA_ELEMENT_TYPE_F32_LE, 1,
			shape, &format) == 0);
	spa_assert_se(pyrtc_stream_create(name, &format, &writer) == -EEXIST);
	probe = shm_open(metadata_name, O_RDONLY, 0);
	spa_assert_se(probe >= 0);
	spa_assert_se(close(probe) == 0);
	spa_assert_se(shm_open(data_name, O_RDONLY, 0) < 0 && errno == ENOENT);
	spa_assert_se(close(fd) == 0);
	spa_assert_se(shm_unlink(metadata_name) == 0);
}

int main(void)
{
	test_dtype_mapping();
	test_roundtrip();
	test_raw_pyrtc_stream();
	test_metadata_collision();
	return 0;
}
