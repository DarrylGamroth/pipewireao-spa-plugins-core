/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <spa/buffer/meta.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/node.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/parser.h>
#include <spa/support/plugin.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/pyrtc.h>

#define HEIGHT 2u
#define WIDTH 3u
#define SAMPLE_COUNT (HEIGHT * WIDTH)

static const char schema[] = "org.pipewireao.test.image/1";

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data data;
	struct spa_chunk chunk;
	struct spa_meta metas[2];
	struct spa_meta_header header;
	uint64_t properties[512];
	uint16_t samples[SAMPLE_COUNT];
};

struct raw_stream {
	char data_name[160];
	char metadata_name[166];
	int data_fd;
	int metadata_fd;
	uint16_t *data;
	double *metadata;
};

static void make_name(char *name, size_t size, const char *kind)
{
	spa_assert_se(snprintf(name, size, "pwao_pyrtc_node_%ld_%s",
			(long)getpid(), kind) < (int)size);
}

static void raw_open(struct raw_stream *stream, const char *name, bool create)
{
	int flags = create ? O_CREAT | O_EXCL | O_RDWR : O_RDWR;

	memset(stream, 0, sizeof(*stream));
	stream->data_fd = -1;
	stream->metadata_fd = -1;
	spa_assert_se(snprintf(stream->data_name, sizeof(stream->data_name),
			"/%s", name) < (int)sizeof(stream->data_name));
	spa_assert_se(snprintf(stream->metadata_name, sizeof(stream->metadata_name),
			"/%s_meta", name) < (int)sizeof(stream->metadata_name));
	stream->data_fd = shm_open(stream->data_name, flags, 0600);
	stream->metadata_fd = shm_open(stream->metadata_name, flags, 0600);
	spa_assert_se(stream->data_fd >= 0 && stream->metadata_fd >= 0);
	if (create) {
		spa_assert_se(ftruncate(stream->data_fd,
				(off_t)(SAMPLE_COUNT * sizeof(uint16_t))) == 0);
		spa_assert_se(ftruncate(stream->metadata_fd,
				(off_t)(10u * sizeof(double))) == 0);
	}
	stream->data = mmap(NULL, SAMPLE_COUNT * sizeof(uint16_t),
			PROT_READ | PROT_WRITE, MAP_SHARED, stream->data_fd, 0);
	stream->metadata = mmap(NULL, 10u * sizeof(double),
			PROT_READ | PROT_WRITE, MAP_SHARED, stream->metadata_fd, 0);
	spa_assert_se(stream->data != MAP_FAILED && stream->metadata != MAP_FAILED);
	if (create) {
		memset(stream->data, 0, SAMPLE_COUNT * sizeof(uint16_t));
		memset(stream->metadata, 0, 10u * sizeof(double));
		stream->metadata[2] = SAMPLE_COUNT * sizeof(uint16_t);
		stream->metadata[3] = 5.0;
		stream->metadata[4] = HEIGHT;
		stream->metadata[5] = WIDTH;
	}
}

static void raw_write(struct raw_stream *stream, const uint16_t *samples)
{
	memcpy(stream->data, samples, SAMPLE_COUNT * sizeof(samples[0]));
	stream->metadata[0] += 1.0;
	stream->metadata[1] = 1000.0 + stream->metadata[0];
}

static void raw_close(struct raw_stream *stream, bool unlink_objects)
{
	spa_assert_se(munmap(stream->metadata, 10u * sizeof(double)) == 0);
	spa_assert_se(munmap(stream->data,
			SAMPLE_COUNT * sizeof(uint16_t)) == 0);
	spa_assert_se(close(stream->metadata_fd) == 0);
	spa_assert_se(close(stream->data_fd) == 0);
	if (unlink_objects) {
		spa_assert_se(shm_unlink(stream->metadata_name) == 0);
		spa_assert_se(shm_unlink(stream->data_name) == 0);
	}
}

static void init_buffer(struct test_buffer *storage, bool output)
{
	memset(storage, 0, sizeof(*storage));
	storage->data.type = SPA_DATA_MemPtr;
	storage->data.fd = -1;
	storage->data.maxsize = sizeof(storage->samples);
	storage->data.data = storage->samples;
	storage->data.chunk = &storage->chunk;
	storage->buffer.n_datas = 1;
	storage->buffer.datas = &storage->data;
	if (output) {
		storage->metas[0] = (struct spa_meta) {
			SPA_META_Header, sizeof(storage->header), &storage->header,
		};
		storage->metas[1] = (struct spa_meta) {
			SPA_META_Properties, 512, storage->properties,
		};
		storage->buffer.n_metas = SPA_N_ELEMENTS(storage->metas);
		storage->buffer.metas = storage->metas;
	}
}

static struct spa_pod *build_format(uint8_t *storage, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { HEIGHT, WIDTH };

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema, SPA_POD_String(schema),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_U16_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR));
}

static struct spa_handle *make_node_access(
		const struct spa_handle_factory *factory, const char *name,
		const char *access, struct spa_node **node)
{
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_PYRTC_NAME, name),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_PYRTC_SCHEMA, schema),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_PYRTC_ACCESS, access),
	};
	const struct spa_dict info = SPA_DICT_INIT(items,
			access == NULL ? 2 : SPA_N_ELEMENTS(items));
	struct spa_handle *handle = calloc(1,
			spa_handle_factory_get_size(factory, &info));

	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)node) == 0);
	return handle;
}

static struct spa_handle *make_node(const struct spa_handle_factory *factory,
		const char *name, struct spa_node **node)
{
	return make_node_access(factory, name, NULL, node);
}

struct metadata_probe {
	uint32_t types;
	bool buffers;
};

static void metadata_result(void *data, int seq SPA_UNUSED, int res,
		uint32_t type, const void *result)
{
	struct metadata_probe *probe = data;
	const struct spa_result_node_params *params = result;
	uint32_t metadata_type;

	spa_assert_se(res == 0 && type == SPA_RESULT_TYPE_NODE_PARAMS);
	if (params->id == SPA_PARAM_Buffers) {
		int32_t mask;
		spa_assert_se(spa_pod_parse_object(params->param, SPA_TYPE_OBJECT_ParamBuffers, NULL,
				SPA_PARAM_BUFFERS_metaType, SPA_POD_Int(&mask)) >= 0);
		spa_assert_se(mask == ((1u << SPA_META_Header) | (1u << SPA_META_Properties)));
		probe->buffers = true;
		return;
	}
	spa_assert_se(params->id == SPA_PARAM_Meta);
	spa_assert_se(spa_pod_parse_object(params->param, SPA_TYPE_OBJECT_ParamMeta, NULL,
			SPA_PARAM_META_type, SPA_POD_Id(&metadata_type)) >= 0);
	spa_assert_se(metadata_type == SPA_META_Header || metadata_type == SPA_META_Properties);
	probe->types |= 1u << metadata_type;
	if (metadata_type == SPA_META_Properties) {
		const struct spa_pod_prop *capacity = spa_pod_find_prop(params->param, NULL, SPA_PARAM_META_size);
		const struct spa_pod_prop *features = spa_pod_find_prop(params->param, NULL, SPA_PARAM_META_features);
		int32_t version;
		spa_assert_se(capacity != NULL && capacity->value.type == SPA_TYPE_Choice);
		spa_assert_se(SPA_POD_CHOICE_VALUE_TYPE(&capacity->value) == SPA_TYPE_Int);
		spa_assert_se(*(const int32_t *)SPA_POD_CHOICE_VALUES(&capacity->value) == 512);
		spa_assert_se(((const int32_t *)SPA_POD_CHOICE_VALUES(&capacity->value))[2] ==
				4096 - sizeof(struct spa_meta_header));
		spa_assert_se(features != NULL && features->flags == SPA_POD_PROP_FLAG_MANDATORY);
		spa_assert_se(spa_pod_get_int(&features->value, &version) == 0 &&
				version == SPA_META_FEATURE_PROPERTIES_VERSION_1);
	}
}

static const struct spa_node_events metadata_events = {
	SPA_VERSION_NODE_EVENTS,
	.result = metadata_result,
};

static void configure_buffers(struct spa_node *node,
		enum spa_direction direction, struct test_buffer storage[2],
		struct spa_buffer *buffers[2], struct spa_io_buffers *io)
{
	uint8_t format_storage[1024];
	struct spa_pod *format = build_format(format_storage,
			sizeof(format_storage));
	uint32_t i;

	spa_assert_se(format != NULL);
	spa_assert_se(spa_node_port_set_param(node, direction, 0,
			SPA_PARAM_Format, 0, format) == 0);
	for (i = 0; i < 2; i++) {
		init_buffer(&storage[i], direction == SPA_DIRECTION_OUTPUT);
		buffers[i] = &storage[i].buffer;
	}
	if (direction == SPA_DIRECTION_OUTPUT) {
		struct metadata_probe probe = { 0 };
		struct spa_hook listener;
		spa_assert_se(spa_node_add_listener(node, &listener, &metadata_events, &probe) == 0);
		spa_assert_se(spa_node_port_enum_params(node, 1, direction, 0,
				SPA_PARAM_Meta, 0, UINT32_MAX, NULL) == 0);
		spa_assert_se(spa_node_port_enum_params(node, 2, direction, 0,
				SPA_PARAM_Buffers, 0, 1, NULL) == 0);
		spa_assert_se(probe.types == ((1u << SPA_META_Header) | (1u << SPA_META_Properties)) && probe.buffers);
		spa_hook_remove(&listener);
		storage[1].buffer.n_metas = 1;
		spa_assert_se(spa_node_port_use_buffers(node, direction, 0, 0,
				buffers, 2) == -EINVAL);
		storage[1].buffer.n_metas = SPA_N_ELEMENTS(storage[1].metas);
		storage[1].metas[1].size = 4072;
		spa_assert_se(spa_node_port_use_buffers(node, direction, 0, 0,
				buffers, 2) == -EINVAL);
		storage[1].metas[1].size = 4096;
		spa_assert_se(spa_node_port_use_buffers(node, direction, 0, 0,
				buffers, 2) == -EINVAL);
		for (i = 0; i < 2; i++)
			storage[i].metas[1].size = 4064;
	}
	spa_assert_se(spa_node_port_use_buffers(node, direction, 0, 0,
			buffers, 2) == 0);
	spa_assert_se(spa_node_port_set_io(node, direction, 0, SPA_IO_Buffers,
			io, sizeof(*io)) == 0);
}

static void stop_and_clear(struct spa_handle *handle, struct spa_node *node,
		enum spa_direction direction, struct spa_io_buffers *io)
{
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);

	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_set_io(node, direction, 0, SPA_IO_Buffers,
			NULL, 0) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, direction, 0, 0,
			NULL, 0) == 0);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
	(void)io;
}

static void test_sink(const struct spa_handle_factory *factory)
{
	const uint16_t expected[SAMPLE_COUNT] = { 11, 12, 13, 21, 22, 23 };
	struct test_buffer storage[2];
	struct spa_buffer *buffers[2];
	struct spa_io_buffers io = SPA_IO_BUFFERS_INIT;
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct raw_stream stream;
	char name[128];

	make_name(name, sizeof(name), "sink");
	handle = make_node(factory, name, &node);
	configure_buffers(node, SPA_DIRECTION_INPUT, storage, buffers, &io);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	memcpy(storage[0].samples, expected, sizeof(expected));
	storage[0].chunk.size = sizeof(expected);
	storage[0].chunk.stride = WIDTH * sizeof(uint16_t);
	io.buffer_id = 0;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(io.status == SPA_STATUS_NEED_DATA);
	raw_open(&stream, name, false);
	spa_assert_se(stream.metadata[0] == 1.0 && stream.metadata[2] ==
			sizeof(expected) && stream.metadata[3] == 5.0 &&
			stream.metadata[4] == HEIGHT && stream.metadata[5] == WIDTH);
	spa_assert_se(memcmp(stream.data, expected, sizeof(expected)) == 0);
	raw_close(&stream, false);
	stop_and_clear(handle, node, SPA_DIRECTION_INPUT, &io);
	spa_assert_se(shm_open(stream.data_name, O_RDONLY, 0) < 0 &&
			errno == ENOENT);
}

static void test_source(const struct spa_handle_factory *factory)
{
	const uint16_t initial[SAMPLE_COUNT] = { 1, 2, 3, 4, 5, 6 };
	const uint16_t updated[SAMPLE_COUNT] = { 7, 8, 9, 10, 11, 12 };
	struct test_buffer storage[2];
	struct spa_buffer *buffers[2];
	struct spa_io_buffers io = SPA_IO_BUFFERS_INIT;
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct raw_stream stream;
	char name[128];
	uint32_t first_id;

	make_name(name, sizeof(name), "source");
	raw_open(&stream, name, true);
	raw_write(&stream, initial);
	handle = make_node(factory, name, &node);
	configure_buffers(node, SPA_DIRECTION_OUTPUT, storage, buffers, &io);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA && io.buffer_id < 2);
	first_id = io.buffer_id;
	spa_assert_se(memcmp(storage[first_id].samples, initial, sizeof(initial)) == 0);
	spa_assert_se(storage[first_id].header.seq == 1);
	spa_assert_se((storage[first_id].header.flags &
			SPA_META_HEADER_FLAG_DISCONT) != 0);
	spa_assert_se(spa_meta_properties_is_valid(&storage[first_id].metas[1], NULL, 0));
	spa_assert_se(((struct spa_pod *)storage[first_id].properties)->size ==
			sizeof(struct spa_pod_object_body));
	for (uint32_t i = 0; i < 2; i++) {
		uint32_t trigger = SPA_META_TRIGGER_HIGH;
		spa_assert_se(spa_meta_properties_add(&storage[i].metas[1],
				SPA_META_PROPERTY_triggerClassification, 0,
				SPA_TYPE_Id, sizeof(trigger), &trigger));
	}
	io.status = SPA_STATUS_NEED_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	raw_write(&stream, updated);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(io.buffer_id < 2);
	spa_assert_se(memcmp(storage[io.buffer_id].samples, updated,
			sizeof(updated)) == 0);
	spa_assert_se(storage[io.buffer_id].header.seq == 2);
	spa_assert_se(spa_meta_properties_is_valid(&storage[io.buffer_id].metas[1], NULL, 0));
	spa_assert_se(spa_meta_properties_find(&storage[io.buffer_id].metas[1],
			SPA_META_PROPERTY_triggerClassification) == NULL);
	stop_and_clear(handle, node, SPA_DIRECTION_OUTPUT, &io);
	raw_close(&stream, true);
}

static void test_attached_sink(const struct spa_handle_factory *factory)
{
	const uint16_t expected[SAMPLE_COUNT] = { 31, 32, 33, 41, 42, 43 };
	struct test_buffer storage[2];
	struct spa_buffer *buffers[2];
	struct spa_io_buffers io = SPA_IO_BUFFERS_INIT;
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct raw_stream stream;
	char name[128];
	int probe;

	make_name(name, sizeof(name), "attached_sink");
	raw_open(&stream, name, true);
	handle = make_node_access(factory, name, SPA_PYRTC_ACCESS_ATTACH, &node);
	configure_buffers(node, SPA_DIRECTION_INPUT, storage, buffers, &io);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	memcpy(storage[0].samples, expected, sizeof(expected));
	storage[0].chunk.size = sizeof(expected);
	storage[0].chunk.stride = WIDTH * sizeof(uint16_t);
	io.buffer_id = 0;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(stream.metadata[0] == 1.0 &&
			memcmp(stream.data, expected, sizeof(expected)) == 0);
	stop_and_clear(handle, node, SPA_DIRECTION_INPUT, &io);
	probe = shm_open(stream.data_name, O_RDONLY, 0);
	spa_assert_se(probe >= 0);
	spa_assert_se(close(probe) == 0);
	raw_close(&stream, true);
}

int main(int argc, char **argv)
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *source = NULL, *sink = NULL;
	uint32_t index = 0;
	void *library, *symbol;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL && sizeof(enumerate) == sizeof(symbol));
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	spa_assert_se(enumerate(&source, &index) == 1 && source != NULL &&
			spa_streq(source->name, SPA_NAME_API_PYRTC_SOURCE));
	spa_assert_se(enumerate(&sink, &index) == 1 && sink != NULL &&
			spa_streq(sink->name, SPA_NAME_API_PYRTC_SINK));
	test_sink(sink);
	test_source(source);
	test_attached_sink(sink);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
