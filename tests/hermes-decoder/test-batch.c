/* SPDX-License-Identifier: MIT */
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/ndarray-utils.h>
#include <spa/support/plugin.h>

#define WIDTH 64u
#define HEIGHT 32u
#define PLANES 4u
#define PIXELS (WIDTH * HEIGHT)

struct batch_buffer {
	struct spa_buffer buffer;
	struct spa_meta meta;
	struct spa_meta_header header;
	struct spa_data data;
	struct spa_chunk chunk;
	_Alignas(8) uint8_t payload[PLANES * (PIXELS * 2 + 8)];
};

static void init_buffer(struct batch_buffer *storage)
{
	*storage = (struct batch_buffer) {
		.meta = { SPA_META_Header, sizeof(storage->header), &storage->header },
		.data = {
			.type = SPA_DATA_MemPtr, .fd = -1, .data = storage->payload,
			.maxsize = sizeof(storage->payload), .chunk = &storage->chunk,
		},
		.buffer = { .n_metas = 1, .metas = &storage->meta, .n_datas = 1, .datas = &storage->data },
	};
}

static void select_format(struct spa_node *node, enum spa_direction direction,
		struct spa_pod_builder *builder, uint32_t bytes)
{
	struct spa_pod *param = NULL;
	struct spa_ndarray_info format = SPA_NDARRAY_INFO_INIT();
	uint32_t index = 0;

	spa_assert_se(spa_node_port_enum_params_sync(node, direction, 0, SPA_PARAM_EnumFormat,
			&index, NULL, &param, builder) == 1);
	spa_assert_se(spa_format_ndarray_parse(param, &format) >= 0);
	spa_assert_se(format.shape[0] == 2 && format.shape[1] == 2);
	if (direction == SPA_DIRECTION_INPUT) {
		spa_assert_se(format.element_type == SPA_ELEMENT_TYPE_U8);
		spa_assert_se(format.n_dimensions == 3 && format.shape[2] == PIXELS * bytes);
	} else {
		spa_assert_se(format.element_type == (bytes == 1 ? SPA_ELEMENT_TYPE_U8 : SPA_ELEMENT_TYPE_U16_LE));
		spa_assert_se(format.n_dimensions == 4 && format.shape[2] == HEIGHT && format.shape[3] == WIDTH);
	}
	spa_assert_se(spa_node_port_set_param(node, direction, 0, SPA_PARAM_Format,
			SPA_NODE_PARAM_FLAG_TEST_ONLY, param) == 0);
	spa_assert_se(spa_node_port_set_param(node, direction, 0, SPA_PARAM_Format, 0, param) == 0);
}

static uint16_t expected_sample(uint32_t plane, uint32_t index, uint32_t bytes)
{
	return bytes == 1 ? (index * 7 + plane * 23) % 251 :
			(uint16_t)(0x1200 + index * 3 + plane * 4096);
}

static void fill_carriers(struct batch_buffer *storage, uint32_t bytes, uint32_t stride)
{
	for (uint32_t p = 0; p < PLANES; p++) {
		for (uint32_t i = 0; i < PIXELS; i++) {
			uint32_t wire_index;
			if (i < PIXELS / 2)
				wire_index = 2 * i;
			else {
				uint32_t j = i - PIXELS / 2;
				wire_index = PIXELS - (j / 32 + 1) * 64 + (j % 32) * 2 + 1;
			}
			uint16_t sample = expected_sample(p, i, bytes);
			uint8_t *dest = storage->payload + p * stride + wire_index * bytes;
			dest[0] = sample & 0xff;
			if (bytes == 2)
				dest[1] = sample >> 8;
		}
	}
	storage->chunk.size = (PLANES - 1) * stride + PIXELS * bytes;
	storage->chunk.stride = stride;
	storage->header = (struct spa_meta_header) { .flags = SPA_META_HEADER_FLAG_DISCONT, .seq = 42, .pts = 1234 };
}

static void test_batch(const struct spa_handle_factory *factory, uint32_t bytes)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("api.hermes.bits-per-pixel", bytes == 1 ? "8" : "16"),
		SPA_DICT_ITEM_INIT("api.hermes.frames-per-buffer", "2"),
		SPA_DICT_ITEM_INIT("api.hermes.counters", "2"),
	};
	const struct spa_dict options = SPA_DICT_INIT_ARRAY(items);
	const struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	const struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	const struct spa_command flush = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Flush);
	struct spa_handle *handle = calloc(1, factory->get_size(factory, &options));
	struct spa_io_buffers input_io = SPA_IO_BUFFERS_INIT, output_io = SPA_IO_BUFFERS_INIT;
	struct batch_buffer input, output;
	struct spa_buffer *inputs[] = { &input.buffer }, *outputs[] = { &output.buffer };
	_Alignas(8) uint8_t params[4096];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(params, sizeof(params));
	struct spa_node *node = NULL;
	struct spa_pod *param = NULL;
	uint32_t index = 0, node_io;
	void *unused;
	const uint32_t stride = PIXELS * bytes + 8;

	spa_assert_se(handle != NULL);
	spa_assert_se(factory->init(factory, NULL, &options, NULL, 0) == -EINVAL);
	spa_assert_se(factory->init(factory, handle, &options, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, "unknown", &unused) == -ENOENT);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node, (void **)&node) == 0);
	spa_assert_se(spa_node_enum_params_sync(node, SPA_PARAM_IO, &index, NULL, &param, &builder) == 1);
	spa_assert_se(spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamIO, NULL,
			SPA_PARAM_IO_id, SPA_POD_Id(&node_io)) >= 0 && node_io == SPA_IO_Position);
	spa_assert_se(spa_node_enum_params_sync(node, SPA_PARAM_IO, &index, NULL, &param, &builder) == 0);
	spa_assert_se(spa_node_enum_params(node, 0, SPA_PARAM_Props, 0, 1, NULL) == -ENOENT);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, NULL) == -ENOENT);
	struct spa_pod *props = spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, props) == -ENOENT);
	spa_assert_se(spa_node_add_port(node, SPA_DIRECTION_INPUT, 1, NULL) == -ENOTSUP);
	spa_assert_se(spa_node_remove_port(node, SPA_DIRECTION_INPUT, 0) == -ENOTSUP);
	spa_assert_se(spa_node_send_command(node, &flush) == -ENOTSUP);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);

	select_format(node, SPA_DIRECTION_INPUT, &builder, bytes);
	select_format(node, SPA_DIRECTION_OUTPUT, &builder, bytes);
	init_buffer(&input);
	init_buffer(&output);
	fill_carriers(&input, bytes, stride);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0,
			SPA_NODE_BUFFERS_FLAG_ALLOC, inputs, 1) == -ENOTSUP);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0, NULL, 1) == -ENOSPC);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0, inputs, 1) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0, outputs, 1) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0, SPA_IO_Buffers, &input_io, sizeof(input_io)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_OUTPUT, 0, SPA_IO_Buffers, &output_io, sizeof(output_io)) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0, SPA_PARAM_Format, 0, NULL) == -EBUSY);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0, SPA_IO_Buffers, NULL, 0) == -EBUSY);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0, NULL, 0) == -EBUSY);
	spa_assert_se(spa_node_port_reuse_buffer(node, 0, 1) == -EINVAL);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);

	input_io.buffer_id = 0;
	input_io.status = SPA_STATUS_HAVE_DATA;
	uint32_t size = input.chunk.size;
	input.chunk.size--;
	spa_assert_se(spa_node_process(node) == -EINVAL && input_io.status == -EINVAL);
	spa_assert_se(output_io.status != SPA_STATUS_HAVE_DATA);
	input.chunk.size = size;
	input_io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(input_io.status == SPA_STATUS_NEED_DATA && output_io.buffer_id == 0);
	spa_assert_se(output.header.seq == 42 && output.header.pts == 1234);
	spa_assert_se(output.header.flags == SPA_META_HEADER_FLAG_DISCONT);
	spa_assert_se(output.chunk.size == PLANES * PIXELS * bytes && output.chunk.stride == (int32_t)(WIDTH * bytes));
	for (uint32_t p = 0; p < PLANES; p++)
		for (uint32_t i = 0; i < PIXELS; i++) {
			uint8_t *sample = output.payload + (p * PIXELS + i) * bytes;
			uint16_t value = sample[0] | (bytes == 2 ? (uint16_t)sample[1] << 8 : 0);
			spa_assert_se(value == expected_sample(p, i, bytes));
		}
	input_io.status = SPA_STATUS_HAVE_DATA;
	input.header.seq = 43;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA);
	spa_assert_se(input_io.status == SPA_STATUS_HAVE_DATA && output.header.seq == 42);
	/* Explicit reuse works even when the host already cleared the shared I/O ID. */
	output_io = SPA_IO_BUFFERS_INIT;
	spa_assert_se(spa_node_port_reuse_buffer(node, 0, 0) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_HAVE_DATA && output.header.seq == 43);
	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_OUTPUT, 0, 0, NULL, 0) == 0);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
}

int main(int argc, char *argv[])
{
	spa_assert_se(argc == 2);
	void *library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	spa_handle_factory_enum_func_t enumerate;
	void *symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL && sizeof(enumerate) == sizeof(symbol));
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	const struct spa_handle_factory *factory = NULL;
	const struct spa_interface_info *interface = NULL;
	uint32_t index = 0;
	spa_assert_se(enumerate(&factory, &index) == 1 && spa_streq(factory->name, "api.hermes.decoder"));
	index = 0;
	spa_assert_se(factory->enum_interface_info(factory, &interface, &index) == 1);
	spa_assert_se(spa_streq(interface->type, SPA_TYPE_INTERFACE_Node));
	spa_assert_se(factory->enum_interface_info(factory, &interface, &index) == 0);
	test_batch(factory, 1);
	test_batch(factory, 2);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
