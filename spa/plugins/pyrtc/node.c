/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/buffer/meta.h>
#include <spa/monitor/device.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/param/ndarray-utils.h>
#include <spa/pod/filter.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/pod.h>

#include "../image-frame.h"
#include "pyrtc.h"
#include "stream.h"

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "pyRTC ndarray transport currently requires little endian"
#endif

#define MIN_BUFFERS 2u
#define MAX_BUFFERS 64u
#define TEXT_SIZE 256u
#define NODE_TEXT_SIZE (TEXT_SIZE + 32u)

enum buffer_state {
	BUFFER_AVAILABLE,
	BUFFER_PUBLISHED,
};

struct port_buffer {
	struct spa_buffer *buffer;
	enum buffer_state state;
};

struct port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[5];
	struct spa_io_buffers *io;
	struct port_buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t scan_hint;
	bool have_format;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	uint64_t info_all;
	struct spa_node_info info;
	struct spa_dict props;
	struct spa_dict_item prop_items[11];
	char name[TEXT_SIZE];
	char schema[TEXT_SIZE];
	char profile[TEXT_SIZE];
	char access[16];
	char node_name[NODE_TEXT_SIZE];
	char description[NODE_TEXT_SIZE];
	struct port port;
	struct pyrtc_stream *stream;
	struct pyrtc_format format;
	enum spa_direction direction;
	bool attach;
	bool initial_pending;
	bool started;
};

static int copy_required(char *destination, size_t size, const char *source)
{
	size_t length;

	if (destination == NULL || size == 0 || source == NULL || source[0] == '\0')
		return -EINVAL;
	length = strlen(source);
	if (length >= size)
		return -ENAMETOOLONG;
	memcpy(destination, source, length + 1u);
	return 0;
}

static int copy_optional(char *destination, size_t size, const char *source)
{
	if (source == NULL) {
		destination[0] = '\0';
		return 0;
	}
	return copy_required(destination, size, source);
}

static void emit_node_info(struct impl *self, bool full)
{
	uint64_t old = full ? self->info.change_mask : 0;

	if (full)
		self->info.change_mask = self->info_all;
	if (self->info.change_mask != 0) {
		spa_node_emit_info(&self->hooks, &self->info);
		self->info.change_mask = old;
	}
}

static void emit_port_info(struct impl *self, bool full)
{
	uint64_t old = full ? self->port.info.change_mask : 0;

	if (full)
		self->port.info.change_mask = self->port.info_all;
	if (self->port.info.change_mask != 0) {
		spa_node_emit_port_info(&self->hooks, self->direction, 0,
				&self->port.info);
		self->port.info.change_mask = old;
	}
}

static int node_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct impl *self = object;
	struct spa_hook_list save;

	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_node_info(self, true);
	emit_port_info(self, true);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

static int node_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *self = object;

	self->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static int node_enum_params(void *object SPA_UNUSED, int seq SPA_UNUSED,
		uint32_t id SPA_UNUSED, uint32_t start SPA_UNUSED,
		uint32_t num SPA_UNUSED, const struct spa_pod *filter SPA_UNUSED)
{
	return -ENOENT;
}

static int node_set_param(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		uint32_t flags SPA_UNUSED, const struct spa_pod *param SPA_UNUSED)
{
	return -ENOENT;
}

static int node_set_io(void *object SPA_UNUSED, uint32_t id SPA_UNUSED,
		void *data SPA_UNUSED, size_t size SPA_UNUSED)
{
	return -ENOENT;
}

static int node_add_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, const struct spa_dict *props SPA_UNUSED)
{
	return -ENOTSUP;
}

static int node_remove_port(void *object SPA_UNUSED,
		enum spa_direction direction SPA_UNUSED, uint32_t port_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static struct spa_pod *build_fixed_format(struct impl *self,
		struct spa_pod_builder *builder, uint32_t id)
{
	struct spa_pod_frame object;
	int32_t shape[PYRTC_MAX_DIMENSIONS];
	uint32_t i;

	for (i = 0; i < self->format.rank; i++)
		shape[i] = (int32_t)self->format.shape[i];

	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema, SPA_POD_String(self->schema),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(self->format.element_type),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, self->format.rank, shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR), 0);
	return spa_pod_builder_pop(builder, &object);
}

static struct spa_pod *build_create_format(struct impl *self,
		struct spa_pod_builder *builder, uint32_t id)
{
	struct spa_pod_frame object;

	spa_pod_builder_push_object(builder, &object, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema, SPA_POD_String(self->schema),
			SPA_FORMAT_NDARRAY_elementType, SPA_POD_CHOICE_ENUM_Id(14,
					SPA_ELEMENT_TYPE_F32_LE,
					SPA_ELEMENT_TYPE_BOOL8,
					SPA_ELEMENT_TYPE_U8, SPA_ELEMENT_TYPE_I8,
					SPA_ELEMENT_TYPE_U16_LE, SPA_ELEMENT_TYPE_I16_LE,
					SPA_ELEMENT_TYPE_U32_LE, SPA_ELEMENT_TYPE_I32_LE,
					SPA_ELEMENT_TYPE_U64_LE, SPA_ELEMENT_TYPE_I64_LE,
					SPA_ELEMENT_TYPE_F16_LE, SPA_ELEMENT_TYPE_F64_LE,
					SPA_ELEMENT_TYPE_COMPLEX_F32_LE,
					SPA_ELEMENT_TYPE_COMPLEX_F64_LE),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR), 0);
	return spa_pod_builder_pop(builder, &object);
}

static int build_port_param(struct impl *self, uint32_t id, uint32_t index,
		struct spa_pod_builder *builder, struct spa_pod **param)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
		if (index > 0)
			return 0;
		*param = self->direction == SPA_DIRECTION_INPUT && !self->attach ?
				build_create_format(self, builder, id) :
				build_fixed_format(self, builder, id);
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Format:
		if (index > 0 || !self->port.have_format)
			return 0;
		*param = build_fixed_format(self, builder, id);
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Buffers:
		if (index > 0 || !self->port.have_format)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(8, MIN_BUFFERS, MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_Int((int32_t)self->format.bytes),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_Int((int32_t)self->format.stride),
				SPA_PARAM_BUFFERS_metaType,
				SPA_POD_Int(self->direction == SPA_DIRECTION_OUTPUT ?
					(1u << SPA_META_Header) : 0),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int((1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_Meta:
		if (index > 0 || self->direction != SPA_DIRECTION_OUTPUT)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size,
				SPA_POD_Int(sizeof(struct spa_meta_header)));
		return *param == NULL ? -ENOSPC : 1;
	case SPA_PARAM_IO:
		if (index > 0)
			return 0;
		*param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers)));
		return *param == NULL ? -ENOSPC : 1;
	default:
		return -ENOENT;
	}
}

static int port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *self = object;
	struct spa_result_node_params result = { .id = id, .next = start };
	uint8_t storage[2048];
	uint32_t count = 0;

	spa_return_val_if_fail(direction == self->direction && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	while (count < num) {
		struct spa_pod_builder builder;
		struct spa_pod *param = NULL;
		int res;

		result.index = result.next++;
		spa_pod_builder_init(&builder, storage, sizeof(storage));
		res = build_port_param(self, id, result.index, &builder, &param);
		if (res <= 0)
			return res;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&self->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

static int validate_schema(struct impl *self, const struct spa_pod *format)
{
	const struct spa_pod_prop *property;
	const char *value;

	if (spa_ndarray_format_key_count(format, SPA_FORMAT_NDARRAY_schema) != 1)
		return -EINVAL;
	property = spa_pod_find_prop(format, NULL, SPA_FORMAT_NDARRAY_schema);
	return property != NULL &&
			spa_pod_get_string(&property->value, &value) == 0 &&
			spa_streq(value, self->schema) ? 0 : -EINVAL;
}

static int parse_selected_format(struct impl *self, const struct spa_pod *param,
		struct pyrtc_format *selected)
{
	uint8_t fixed_storage[2048];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(fixed_storage,
			sizeof(fixed_storage));
	struct spa_ndarray_info ndarray = SPA_NDARRAY_INFO_INIT();
	const struct spa_pod *fixed;
	uint32_t shape[PYRTC_MAX_DIMENSIONS] = { 0 };
	int res;

	fixed = pipewireao_pod_unwrap_fixed_choices(&builder, param);
	if (fixed == NULL || spa_format_ndarray_parse(fixed, &ndarray) < 0 ||
			ndarray.layout != SPA_NDARRAY_LAYOUT_ROW_MAJOR ||
			ndarray.n_dimensions == 0 ||
			ndarray.n_dimensions > PYRTC_MAX_DIMENSIONS ||
			(res = validate_schema(self, fixed)) < 0)
		return -EINVAL;
	memcpy(shape, ndarray.shape,
			ndarray.n_dimensions * sizeof(ndarray.shape[0]));
	return pyrtc_format_from_element(ndarray.element_type,
			ndarray.n_dimensions, shape, selected);
}

static bool same_format(const struct pyrtc_format *left,
		const struct pyrtc_format *right)
{
	return left->element_type == right->element_type &&
			left->rank == right->rank &&
			memcmp(left->shape, right->shape,
					left->rank * sizeof(left->shape[0])) == 0;
}

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *self = object;
	struct pyrtc_format selected;
	int res;

	spa_return_val_if_fail(direction == self->direction && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (self->started || self->port.n_buffers != 0)
		return -EBUSY;
	if (param == NULL) {
		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
			if (self->direction == SPA_DIRECTION_INPUT && !self->attach &&
					self->stream != NULL) {
				pyrtc_stream_close(self->stream);
				self->stream = NULL;
			}
			self->port.have_format = false;
			if (self->direction == SPA_DIRECTION_INPUT && !self->attach)
				memset(&self->format, 0, sizeof(self->format));
		}
		return 0;
	}
	if ((res = parse_selected_format(self, param, &selected)) < 0)
		return res;
	if ((self->direction == SPA_DIRECTION_OUTPUT || self->attach) &&
			!same_format(&selected, &self->format))
		return -EINVAL;
	if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
		self->format = selected;
		self->port.have_format = true;
		self->port.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
		self->port.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
		self->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
		emit_port_info(self, false);
	}
	return 0;
}

static int release_buffers(struct impl *self)
{
	if (self->started)
		return -EBUSY;
	memset(self->port.buffers, 0, sizeof(self->port.buffers));
	self->port.n_buffers = 0;
	self->port.scan_hint = 0;
	return 0;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t flags SPA_UNUSED,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *self = object;
	uint32_t i;

	spa_return_val_if_fail(direction == self->direction && port_id == 0,
			-EINVAL);
	if (self->started)
		return -EBUSY;
	if (n_buffers == 0)
		return self->port.n_buffers == 0 ? 0 : release_buffers(self);
	if (!self->port.have_format || self->port.n_buffers != 0 ||
			buffers == NULL || n_buffers < MIN_BUFFERS || n_buffers > MAX_BUFFERS)
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		struct spa_data *data;

		if (buffers[i] == NULL || buffers[i]->n_datas != 1 ||
				(data = &buffers[i]->datas[0])->data == NULL ||
				(data->type != SPA_DATA_MemPtr && data->type != SPA_DATA_MemFd) ||
				data->maxsize < self->format.bytes || data->chunk == NULL ||
				(self->direction == SPA_DIRECTION_OUTPUT &&
				 (spa_buffer_find_meta_data(buffers[i], SPA_META_Header,
						sizeof(struct spa_meta_header)) == NULL ||
				  !spa_meta_records_validate(buffers[i]->metas, buffers[i]->n_metas, NULL, 0))))
			return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++) {
		self->port.buffers[i].buffer = buffers[i];
		self->port.buffers[i].state = BUFFER_AVAILABLE;
	}
	self->port.n_buffers = n_buffers;
	return 0;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;

	spa_return_val_if_fail(direction == self->direction && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_Buffers)
		return -ENOENT;
	if (data != NULL && size < sizeof(struct spa_io_buffers))
		return -ENOSPC;
	self->port.io = data;
	return 0;
}

static int port_reuse_buffer(void *object SPA_UNUSED,
		uint32_t port_id SPA_UNUSED, uint32_t buffer_id SPA_UNUSED)
{
	return -ENOTSUP;
}

static int recycle_output(struct impl *self)
{
	struct spa_io_buffers *io = self->port.io;
	uint32_t id;

	if (io == NULL || io->status == SPA_STATUS_HAVE_DATA ||
			io->buffer_id == SPA_ID_INVALID)
		return 0;
	id = io->buffer_id;
	if (id >= self->port.n_buffers ||
			self->port.buffers[id].state != BUFFER_PUBLISHED)
		return -EPROTO;
	self->port.buffers[id].state = BUFFER_AVAILABLE;
	self->port.scan_hint = (id + 1u) % self->port.n_buffers;
	io->buffer_id = SPA_ID_INVALID;
	return 0;
}

static struct port_buffer *take_output(struct impl *self)
{
	uint32_t offset;

	for (offset = 0; offset < self->port.n_buffers; offset++) {
		uint32_t id = (self->port.scan_hint + offset) % self->port.n_buffers;

		if (self->port.buffers[id].state == BUFFER_AVAILABLE) {
			self->port.scan_hint = (id + 1u) % self->port.n_buffers;
			return &self->port.buffers[id];
		}
	}
	return NULL;
}

static int process_source(struct impl *self)
{
	struct pwao_image_frame publication;
	struct port_buffer *output;
	struct spa_data *data;
	uint64_t sequence;
	bool available;
	uint32_t id;
	int res;

	if ((res = recycle_output(self)) < 0)
		return res;
	if (self->port.io->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;
	if ((res = pyrtc_stream_has_update(self->stream, self->initial_pending,
			&available)) < 0)
		return res;
	if (!available)
		return SPA_STATUS_OK;
	output = take_output(self);
	if (output == NULL)
		return SPA_STATUS_OK;
	id = (uint32_t)(output - self->port.buffers);
	data = &output->buffer->datas[0];
	if ((res = pyrtc_stream_read(self->stream, data->data,
			self->format.bytes, &sequence)) < 0)
		return res == -EAGAIN ? SPA_STATUS_OK : res;
	publication = (struct pwao_image_frame) {
		.data_index = 0,
		.header_flags = self->initial_pending ? SPA_META_HEADER_FLAG_DISCONT : 0,
		.offset = 0,
		.size = (uint32_t)self->format.bytes,
		.stride = (int32_t)self->format.stride,
		.sequence = sequence,
		.pts = SPA_TIME_INVALID,
	};
	if ((res = pwao_image_frame_write(output->buffer, &publication,
			PWAO_IMAGE_FRAME_REQUIRE_HEADER)) < 0)
		return res;
	output->state = BUFFER_PUBLISHED;
	self->port.io->buffer_id = id;
	self->port.io->status = SPA_STATUS_HAVE_DATA;
	self->initial_pending = false;
	return SPA_STATUS_HAVE_DATA;
}

static int process_sink(struct impl *self)
{
	struct spa_io_buffers *io = self->port.io;
	struct spa_data *data;
	const uint8_t *source;
	uint32_t id, offset;
	int res;

	if (io->status != SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_NEED_DATA;
	id = io->buffer_id;
	if (id >= self->port.n_buffers || self->port.buffers[id].buffer == NULL)
		return -EPROTO;
	data = &self->port.buffers[id].buffer->datas[0];
	if (data->data == NULL || data->chunk == NULL ||
			(data->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0)
		return -EINVAL;
	offset = data->chunk->offset % data->maxsize;
	if (self->format.bytes > data->maxsize - offset ||
			data->chunk->size < self->format.bytes ||
			(data->chunk->stride != 0 &&
			 data->chunk->stride != (int32_t)self->format.stride))
		return -EINVAL;
	source = SPA_PTROFF(data->data, offset, const uint8_t);
	res = pyrtc_stream_write(self->stream, source, self->format.bytes);
	io->status = res < 0 ? res : SPA_STATUS_NEED_DATA;
	return res < 0 ? res : SPA_STATUS_NEED_DATA;
}

static int node_process(void *object)
{
	struct impl *self = object;

	if (!self->started)
		return SPA_STATUS_OK;
	if (self->port.io == NULL)
		return -EIO;
	return self->direction == SPA_DIRECTION_OUTPUT ?
			process_source(self) : process_sink(self);
}

static int stop_node(struct impl *self)
{
	if (!self->started)
		return 0;
	self->started = false;
	return 0;
}

static int node_send_command(void *object, const struct spa_command *command)
{
	struct impl *self = object;
	int res;

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!self->port.have_format || self->port.n_buffers == 0 ||
				self->port.io == NULL)
			return -EIO;
		if (self->started)
			return 0;
		if (self->direction == SPA_DIRECTION_OUTPUT) {
			self->initial_pending = true;
		} else if (!self->attach && self->stream == NULL) {
			if ((res = pyrtc_stream_create(self->name, &self->format,
					&self->stream)) < 0)
				return res;
		}
		self->started = true;
		return 0;
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		return stop_node(self);
	default:
		return -ENOTSUP;
	}
}

static const struct spa_node_methods node_methods = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = node_add_listener,
	.set_callbacks = node_set_callbacks,
	.enum_params = node_enum_params,
	.set_param = node_set_param,
	.set_io = node_set_io,
	.send_command = node_send_command,
	.add_port = node_add_port,
	.remove_port = node_remove_port,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = port_reuse_buffer,
	.process = node_process,
};

static int get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *self = (struct impl *)handle;

	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

static int clear(struct spa_handle *handle)
{
	struct impl *self = (struct impl *)handle;

	(void)stop_node(self);
	if (self->stream != NULL) {
		pyrtc_stream_close(self->stream);
		self->stream = NULL;
	}
	return 0;
}

static size_t get_size(const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_dict *params SPA_UNUSED)
{
	return sizeof(struct impl);
}

static void configure_props(struct impl *self)
{
	uint32_t n = 0;
	const char *kind = self->direction == SPA_DIRECTION_OUTPUT ? "source" : "sink";

	snprintf(self->node_name, sizeof(self->node_name), "pyrtc_%s.%s",
			kind, self->name);
	snprintf(self->description, sizeof(self->description),
			"pyRTC %s %s", self->name, kind);
#define ADD_ITEM(key, value) self->prop_items[n++] = SPA_DICT_ITEM_INIT(key, value)
    ADD_ITEM("pipewireao.header.pts.clock", "unavailable");
    ADD_ITEM("pipewireao.header.pts.absolute-conversion", "unavailable");
    ADD_ITEM(SPA_KEY_DEVICE_API, "pyrtc");
	ADD_ITEM(SPA_KEY_MEDIA_CLASS,
			self->direction == SPA_DIRECTION_OUTPUT ? "Array/Source" : "Array/Sink");
	ADD_ITEM(SPA_KEY_NODE_NAME, self->node_name);
	ADD_ITEM(SPA_KEY_NODE_DESCRIPTION, self->description);
	if (self->direction == SPA_DIRECTION_OUTPUT)
		ADD_ITEM(SPA_KEY_NODE_DRIVER, "true");
	ADD_ITEM(SPA_KEY_API_PYRTC_NAME, self->name);
	ADD_ITEM(SPA_KEY_API_PYRTC_SCHEMA, self->schema);
	ADD_ITEM(SPA_KEY_API_PYRTC_ACCESS, self->access);
	if (self->profile[0] != '\0')
		ADD_ITEM(SPA_KEY_API_PYRTC_PROFILE, self->profile);
#undef ADD_ITEM
	self->props = SPA_DICT_INIT(self->prop_items, n);
}

static int init(const struct spa_handle_factory *factory,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support SPA_UNUSED,
		uint32_t n_support SPA_UNUSED)
{
	struct impl *self = (struct impl *)handle;
	const char *value;
	bool source;
	int res;

	spa_return_val_if_fail(handle != NULL && factory != NULL, -EINVAL);
	memset(self, 0, sizeof(*self));
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	source = spa_streq(factory->name, SPA_NAME_API_PYRTC_SOURCE);
	self->direction = source ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
	value = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_API_PYRTC_NAME);
	if ((res = copy_required(self->name, sizeof(self->name), value)) < 0)
		return res;
	value = spa_dict_lookup(info, SPA_KEY_API_PYRTC_SCHEMA);
	if ((res = copy_required(self->schema, sizeof(self->schema), value)) < 0)
		return res;
	value = spa_dict_lookup(info, SPA_KEY_API_PYRTC_PROFILE);
	if ((res = copy_optional(self->profile, sizeof(self->profile), value)) < 0)
		return res;
	value = spa_dict_lookup(info, SPA_KEY_API_PYRTC_ACCESS);
	if (source) {
		if (value != NULL && !spa_streq(value, SPA_PYRTC_ACCESS_ATTACH))
			return -EINVAL;
		self->attach = true;
		(void)copy_required(self->access, sizeof(self->access),
				SPA_PYRTC_ACCESS_ATTACH);
	} else {
		if (value == NULL || spa_streq(value, SPA_PYRTC_ACCESS_CREATE))
			self->attach = false;
		else if (spa_streq(value, SPA_PYRTC_ACCESS_ATTACH))
			self->attach = true;
		else
			return -EINVAL;
		(void)copy_required(self->access, sizeof(self->access), self->attach ?
				SPA_PYRTC_ACCESS_ATTACH : SPA_PYRTC_ACCESS_CREATE);
	}
	if (source || self->attach) {
		if ((res = pyrtc_stream_open(self->name, source, &self->stream,
				&self->format)) < 0)
			return res;
	}
	spa_hook_list_init(&self->hooks);
	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_input_ports = source ? 0 : 1;
	self->info.max_output_ports = source ? 1 : 0;
	self->info.flags = SPA_NODE_FLAG_RT;
	if (source)
		self->info.flags |= SPA_NODE_FLAG_POLL_DRIVER;
	configure_props(self);
	self->info.props = &self->props;
	self->port.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PARAMS;
	self->port.info = SPA_PORT_INFO_INIT();
	self->port.info.flags = SPA_PORT_FLAG_LIVE;
	self->port.params[0] = (struct spa_param_info) {
		.id = SPA_PARAM_EnumFormat,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.params[1] = (struct spa_param_info) {
		.id = SPA_PARAM_Format,
		.flags = SPA_PARAM_INFO_READWRITE,
	};
	self->port.params[2] = (struct spa_param_info) {
		.id = SPA_PARAM_Buffers,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.params[3] = (struct spa_param_info) {
		.id = SPA_PARAM_Meta,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.params[4] = (struct spa_param_info) {
		.id = SPA_PARAM_IO,
		.flags = SPA_PARAM_INFO_READ,
	};
	self->port.info.params = self->port.params;
	self->port.info.n_params = SPA_N_ELEMENTS(self->port.params);
	return 0;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int enum_interface_info(
		const struct spa_handle_factory *factory SPA_UNUSED,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_pyrtc_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_PYRTC_SOURCE,
	NULL,
	get_size,
	init,
	enum_interface_info,
};

const struct spa_handle_factory spa_pyrtc_sink_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_PYRTC_SINK,
	NULL,
	get_size,
	init,
	enum_interface_info,
};
