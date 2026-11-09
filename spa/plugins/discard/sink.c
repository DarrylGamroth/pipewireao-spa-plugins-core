/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <spa/monitor/device.h>
#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/compare.h>
#include <spa/pod/filter.h>
#include <spa/pod/iter.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/discard.h>

#define MAX_BUFFERS 64u
#define NODE_NAME_SIZE 256u
#define NODE_DESCRIPTION_SIZE 256u
#define BUFFER_SIZE_TEXT_SIZE 16u
#define FNV1A_OFFSET_BASIS UINT64_C(14695981039346656037)

struct input_port {
	uint64_t info_all;
	struct spa_port_info info;
	struct spa_param_info params[4];
	struct spa_dict props;
	struct spa_dict_item prop_items[1];
	struct spa_buffer *buffers[MAX_BUFFERS];
	struct spa_io_buffers *io;
	struct spa_pod *format;
	uint32_t n_buffers;
};

struct metrics {
	_Atomic uint64_t buffers;
	_Atomic uint64_t data_blocks;
	_Atomic uint64_t bytes;
	_Atomic uint64_t protocol_errors;
	_Atomic uint64_t process_calls;
	_Atomic uint64_t payload_digest;
	_Atomic uint64_t digest_bytes;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;
	struct spa_loop *data_loop;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;
	uint64_t info_all;
	struct spa_node_info info;
	struct spa_param_info params[3];
	struct spa_dict node_props;
	struct spa_dict_item node_items[5];
	char node_name[NODE_NAME_SIZE];
	char node_description[NODE_DESCRIPTION_SIZE];
	char minimum_buffer_size_text[BUFFER_SIZE_TEXT_SIZE];
	struct input_port input;
	struct metrics metrics;
	uint32_t minimum_buffer_size;
	bool started;
};

struct metric_description {
	uint32_t id;
	const char *name;
	const char *description;
};

static const struct metric_description metric_descriptions[] = {
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_BUFFERS,
		.description = "Buffers discarded since node construction",
	},
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_DATA_BLOCKS,
		.description = "Buffer data blocks discarded since node construction",
	},
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_BYTES,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_BYTES,
		.description = "Chunk bytes advertised by discarded buffers",
	},
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS,
		.description = "Invalid buffer references rejected since node construction",
	},
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_PROCESS_CALLS,
		.description = "Processing callback calls since node construction",
	},
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST,
		.description = "Ordered FNV-1a digest of addressable discarded payload bytes",
	},
	{
		.id = SPA_PROP_PIPEWIREAO_DISCARD_DIGEST_BYTES,
		.name = SPA_PROP_INFO_PIPEWIREAO_DISCARD_DIGEST_BYTES,
		.description = "Discarded payload bytes included in the digest",
	},
};

static int copy_string(char *destination, size_t size, const char *source)
{
	size_t length;

	if (destination == NULL || size == 0 || source == NULL)
		return -EINVAL;
	length = strlen(source);
	if (length >= size)
		return -ENAMETOOLONG;
	memcpy(destination, source, length + 1u);
	return 0;
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
	struct input_port *input = &self->input;
	uint64_t old = full ? input->info.change_mask : 0;

	if (full)
		input->info.change_mask = input->info_all;
	if (input->info.change_mask != 0) {
		spa_node_emit_port_info(&self->hooks, SPA_DIRECTION_INPUT, 0,
				&input->info);
		input->info.change_mask = old;
	}
}

static int add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct impl *self = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_hook_list_isolate(&self->hooks, &save, listener, events, data);
	emit_node_info(self, true);
	emit_port_info(self, true);
	spa_hook_list_join(&self->hooks, &save);
	return 0;
}

static int set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	self->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static uint64_t metric_value(const struct impl *self, uint32_t id)
{
	switch (id) {
	case SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS:
		return atomic_load_explicit(&self->metrics.buffers,
				memory_order_relaxed);
	case SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS:
		return atomic_load_explicit(&self->metrics.data_blocks,
				memory_order_relaxed);
	case SPA_PROP_PIPEWIREAO_DISCARD_BYTES:
		return atomic_load_explicit(&self->metrics.bytes,
				memory_order_relaxed);
	case SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS:
		return atomic_load_explicit(&self->metrics.protocol_errors,
				memory_order_relaxed);
	case SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS:
		return atomic_load_explicit(&self->metrics.process_calls,
				memory_order_relaxed);
	case SPA_PROP_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST:
		return atomic_load_explicit(&self->metrics.payload_digest,
				memory_order_relaxed);
	case SPA_PROP_PIPEWIREAO_DISCARD_DIGEST_BYTES:
		return atomic_load_explicit(&self->metrics.digest_bytes,
				memory_order_relaxed);
	default:
		return 0;
	}
}

static struct spa_pod *build_metric_info(struct spa_pod_builder *builder,
		uint32_t index)
{
	const struct metric_description *metric;

	if (index >= SPA_N_ELEMENTS(metric_descriptions))
		return NULL;
	metric = &metric_descriptions[index];
	return spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo,
			SPA_PROP_INFO_id, SPA_POD_Id(metric->id),
			SPA_PROP_INFO_name, SPA_POD_String(metric->name),
			SPA_PROP_INFO_description, SPA_POD_String(metric->description),
			SPA_PROP_INFO_type, SPA_POD_Long(0),
			SPA_PROP_INFO_params, SPA_POD_Bool(false));
}

static struct spa_pod *build_metrics(struct impl *self,
		struct spa_pod_builder *builder, uint32_t index)
{
	if (index > 0)
		return NULL;
	return spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS)),
			SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS)),
			SPA_PROP_PIPEWIREAO_DISCARD_BYTES,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_BYTES)),
			SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS)),
			SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS)),
			SPA_PROP_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST)),
			SPA_PROP_PIPEWIREAO_DISCARD_DIGEST_BYTES,
			SPA_POD_Long((int64_t)metric_value(self,
					SPA_PROP_PIPEWIREAO_DISCARD_DIGEST_BYTES)));
}

static int enum_params(void *object, int seq, uint32_t id, uint32_t start,
		uint32_t num, const struct spa_pod *filter)
{
	struct impl *self = object;
	uint8_t storage[1024];
	uint32_t count = 0;
	struct spa_result_node_params result = { 0 };

	spa_return_val_if_fail(self != NULL && num > 0, -EINVAL);
	if (id != SPA_PARAM_PropInfo && id != SPA_PARAM_Props &&
			id != SPA_PARAM_IO)
		return -ENOENT;
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
				sizeof(storage));
		struct spa_pod *param;

		result.index = result.next++;
		switch (id) {
		case SPA_PARAM_PropInfo:
			param = build_metric_info(&builder, result.index);
			break;
		case SPA_PARAM_Props:
			param = build_metrics(self, &builder, result.index);
			break;
		case SPA_PARAM_IO:
			param = result.index > 0 ? NULL :
					spa_pod_builder_add_object(&builder,
						SPA_TYPE_OBJECT_ParamIO, SPA_PARAM_IO,
						SPA_PARAM_IO_id,
						SPA_POD_Id(SPA_IO_Position),
						SPA_PARAM_IO_size,
						SPA_POD_Int(sizeof(struct spa_io_position)));
			break;
		default:
			spa_assert_not_reached();
		}
		if (param == NULL)
			return 0;
		if (spa_pod_filter(&builder, &result.param, param, filter) < 0)
			continue;
		spa_node_emit_result(&self->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

static int set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	(void)object;
	(void)flags;
	(void)param;
	return id == SPA_PARAM_Props ? -EPERM : -ENOENT;
}

static int set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	if (id != SPA_IO_Position)
		return -ENOENT;
	if (data != NULL && size < sizeof(struct spa_io_position))
		return -EINVAL;
	/*
	 * Accepting the graph position is the follower-side scheduling handshake.
	 * PipeWire uses a successful update to publish the current driver identity
	 * before it admits this node to a cycle. The discard path does not need to
	 * retain or inspect the position itself.
	 */
	return 0;
}

static int add_port(void *object, enum spa_direction direction,
		uint32_t port_id, const struct spa_dict *props)
{
	(void)object;
	(void)direction;
	(void)port_id;
	(void)props;
	return -ENOTSUP;
}

static int remove_port(void *object, enum spa_direction direction,
		uint32_t port_id)
{
	(void)object;
	(void)direction;
	(void)port_id;
	return -ENOTSUP;
}

static struct spa_pod *build_format(struct impl *self, uint32_t id,
		uint32_t index, struct spa_pod_builder *builder,
		const struct spa_pod *filter)
{
	struct spa_pod_frame frame;

	if (index > 0)
		return NULL;
	if (id == SPA_PARAM_Format) {
		struct spa_pod *result;

		if (self->input.format == NULL)
			return NULL;
		if (spa_pod_builder_raw_padded(builder, self->input.format,
				SPA_POD_SIZE(self->input.format)) < 0)
			return NULL;
		result = spa_pod_builder_deref(builder, 0);
		return result;
	}
	if (filter != NULL && (!spa_pod_is_object(filter) ||
			SPA_POD_OBJECT_TYPE(filter) != SPA_TYPE_OBJECT_Format))
		return NULL;
	spa_pod_builder_push_object(builder, &frame,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	if (filter != NULL) {
		const struct spa_pod_object *object =
				(const struct spa_pod_object *)filter;
		const struct spa_pod_prop *property;

		SPA_POD_OBJECT_FOREACH(object, property)
			if (spa_pod_builder_raw_padded(builder, property,
					SPA_POD_PROP_SIZE(property)) < 0)
				return NULL;
	}
	return spa_pod_builder_pop(builder, &frame);
}

static struct spa_pod *build_port_param(struct impl *self, uint32_t id,
		uint32_t index, struct spa_pod_builder *builder,
		const struct spa_pod *filter)
{
	switch (id) {
	case SPA_PARAM_EnumFormat:
	case SPA_PARAM_Format:
		return build_format(self, id, index, builder, filter);
	case SPA_PARAM_Buffers:
		if (index > 0 || self->input.format == NULL)
			return NULL;
		return spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamBuffers, id,
				SPA_PARAM_BUFFERS_buffers,
				SPA_POD_CHOICE_RANGE_Int(2, 1, (int32_t)MAX_BUFFERS),
				SPA_PARAM_BUFFERS_blocks,
				SPA_POD_CHOICE_RANGE_Int(1, 0, INT32_MAX),
				SPA_PARAM_BUFFERS_size,
				SPA_POD_CHOICE_RANGE_Int(
						(int32_t)self->minimum_buffer_size,
						(int32_t)self->minimum_buffer_size,
						INT32_MAX),
				SPA_PARAM_BUFFERS_stride,
				SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
				SPA_PARAM_BUFFERS_align,
				SPA_POD_CHOICE_RANGE_Int(1, 1, INT32_MAX),
				SPA_PARAM_BUFFERS_dataType,
				SPA_POD_CHOICE_FLAGS_Int(
						(1u << SPA_DATA_MemPtr) |
						(1u << SPA_DATA_MemFd) |
						(1u << SPA_DATA_DmaBuf) |
						(1u << SPA_DATA_MemId) |
						(1u << SPA_DATA_SyncObj)));
	case SPA_PARAM_IO:
		if (index > 0)
			return NULL;
		return spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size,
				SPA_POD_Int(sizeof(struct spa_io_buffers)));
	default:
		return NULL;
	}
}

static int port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *self = object;
	uint8_t storage[4096];
	uint32_t count = 0;
	struct spa_result_node_params result = { 0 };

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	spa_return_val_if_fail(num > 0, -EINVAL);
	if (id != SPA_PARAM_EnumFormat && id != SPA_PARAM_Format &&
			id != SPA_PARAM_Buffers && id != SPA_PARAM_IO)
		return -ENOENT;
	result.id = id;
	result.next = start;
	while (count < num) {
		struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
				sizeof(storage));
		struct spa_pod *param;

		result.index = result.next++;
		param = build_port_param(self, id, result.index, &builder,
				id == SPA_PARAM_EnumFormat ? filter : NULL);
		if (param == NULL)
			return 0;
		if (id != SPA_PARAM_EnumFormat && filter != NULL) {
			if (spa_pod_filter(&builder, &result.param, param,
					filter) < 0)
				continue;
		} else {
			result.param = param;
		}
		spa_node_emit_result(&self->hooks, seq, 0,
				SPA_RESULT_TYPE_NODE_PARAMS, &result);
		count++;
	}
	return 0;
}

static int update_started(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t started, const void *data SPA_UNUSED, size_t size SPA_UNUSED,
		void *object)
{
	struct impl *self = object;

	self->started = started != 0;
	return 0;
}

static int set_started(struct impl *self, bool started)
{
	if (self->data_loop != NULL)
		return spa_loop_locked(self->data_loop, update_started, started,
				NULL, 0, self);
	return update_started(NULL, false, started, NULL, 0, self);
}

static int release_buffers(struct impl *self)
{
	int res;

	/* Link removal can clear the pool before the host sends Pause
	 * or detaches the node's I/O. Wait for in-flight processing. */
	if (self->started && (res = set_started(self, false)) < 0)
		return res;
	memset(self->input.buffers, 0, sizeof(self->input.buffers));
	self->input.n_buffers = 0;
	return 0;
}

static bool valid_format(const struct spa_pod *param)
{
	return param != NULL && spa_pod_is_object(param) &&
			SPA_POD_OBJECT_TYPE(param) == SPA_TYPE_OBJECT_Format &&
			spa_pod_is_fixated(param) == 1;
}

static int port_set_param(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *self = object;
	struct spa_pod *copy;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_PARAM_Format)
		return -ENOENT;
	if (param == NULL) {
		if (self->started || self->input.n_buffers != 0)
			return -EBUSY;
		if (!(flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)) {
			free(self->input.format);
			self->input.format = NULL;
			self->input.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
			self->input.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
			self->input.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
			emit_port_info(self, false);
		}
		return 0;
	}
	if (!valid_format(param))
		return -EINVAL;
	if (self->started || self->input.n_buffers != 0)
		return self->input.format != NULL &&
				spa_pod_compare(self->input.format, param) == 0 ? 0 : -EBUSY;
	if (flags & SPA_NODE_PARAM_FLAG_TEST_ONLY)
		return 0;
	copy = spa_pod_copy(param);
	if (copy == NULL)
		return -ENOMEM;
	SPA_POD_OBJECT_ID(copy) = SPA_PARAM_Format;
	free(self->input.format);
	self->input.format = copy;
	self->input.params[1].flags ^= SPA_PARAM_INFO_SERIAL;
	self->input.params[2].flags ^= SPA_PARAM_INFO_SERIAL;
	self->input.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(self, false);
	return 0;
}

static int port_use_buffers(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t flags, struct spa_buffer **buffers,
		uint32_t n_buffers)
{
	struct impl *self = object;
	uint32_t i;

	(void)flags;
	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	if (n_buffers == 0)
		return release_buffers(self);
	if (self->started)
		return -EBUSY;
	if (self->input.format == NULL || buffers == NULL ||
			n_buffers > MAX_BUFFERS || self->input.n_buffers != 0)
		return -EINVAL;
	for (i = 0; i < n_buffers; i++) {
		if (buffers[i] == NULL ||
				(buffers[i]->n_datas > 0 && buffers[i]->datas == NULL))
			return -EINVAL;
	}
	for (i = 0; i < n_buffers; i++)
		self->input.buffers[i] = buffers[i];
	self->input.n_buffers = n_buffers;
	return 0;
}

static int set_buffer_io(struct spa_loop *loop SPA_UNUSED, bool async SPA_UNUSED,
		uint32_t seq SPA_UNUSED, const void *data, size_t size SPA_UNUSED,
		void *object)
{
	struct impl *self = object;

	self->input.io = *(struct spa_io_buffers *const *)data;
	return 0;
}

static int port_set_io(void *object, enum spa_direction direction,
		uint32_t port_id, uint32_t id, void *data, size_t size)
{
	struct impl *self = object;
	struct spa_io_buffers *io = data;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	spa_return_val_if_fail(direction == SPA_DIRECTION_INPUT && port_id == 0,
			-EINVAL);
	if (id != SPA_IO_Buffers)
		return -ENOENT;
	if (data != NULL && size < sizeof(struct spa_io_buffers))
		return -ENOSPC;
	if (self->data_loop != NULL)
		return spa_loop_locked(self->data_loop, set_buffer_io,
				SPA_ID_INVALID, &io, sizeof(io), self);
	return set_buffer_io(NULL, false, SPA_ID_INVALID, &io, sizeof(io), self);
}

static int reuse_buffer(void *object, uint32_t port_id, uint32_t buffer_id)
{
	(void)object;
	(void)port_id;
	(void)buffer_id;
	return -ENOTSUP;
}

static int send_command(void *object, const struct spa_command *command)
{
	struct impl *self = object;

	spa_return_val_if_fail(self != NULL && command != NULL, -EINVAL);
	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (self->input.format == NULL || self->input.n_buffers == 0 ||
				self->input.io == NULL)
			return -EIO;
		return set_started(self, true);
	case SPA_NODE_COMMAND_Pause:
	case SPA_NODE_COMMAND_Suspend:
		return set_started(self, false);
	default:
		return -ENOTSUP;
	}
}

static uint64_t saturated_sum(uint64_t left, uint64_t right)
{
	const uint64_t maximum = INT64_MAX;

	return left > maximum - SPA_MIN(right, maximum) ? maximum :
			left + SPA_MIN(right, maximum);
}

static void metric_add(_Atomic uint64_t *counter, uint64_t value)
{
	/* SPA serializes process() for one node, so these counters have one writer. */
	uint64_t current = atomic_load_explicit(counter, memory_order_relaxed);

	atomic_store_explicit(counter, saturated_sum(current, value),
			memory_order_relaxed);
}

static void record_buffer(struct impl *self, const struct spa_buffer *buffer)
{
	uint64_t bytes = 0;
	uint64_t digest_bytes = 0;
	uint64_t digest = atomic_load_explicit(&self->metrics.payload_digest,
			memory_order_relaxed);
	uint32_t i;

	for (i = 0; i < buffer->n_datas; i++) {
		const struct spa_data *data = &buffer->datas[i];

		if (data->chunk != NULL) {
			uint32_t offset = data->maxsize == 0 ? 0 :
					data->chunk->offset % data->maxsize;
			uint32_t size = SPA_MIN(data->chunk->size,
					data->maxsize - offset);
			const uint8_t *payload = data->data;
			uint32_t byte;

			bytes = saturated_sum(bytes, data->chunk->size);
			if (payload == NULL)
				continue;
			for (byte = 0; byte < size; byte++) {
				digest ^= payload[offset + byte];
				digest *= UINT64_C(1099511628211);
			}
			digest_bytes = saturated_sum(digest_bytes, size);
		}
	}
	metric_add(&self->metrics.buffers, 1);
	metric_add(&self->metrics.data_blocks, buffer->n_datas);
	metric_add(&self->metrics.bytes, bytes);
	atomic_store_explicit(&self->metrics.payload_digest, digest,
			memory_order_relaxed);
	metric_add(&self->metrics.digest_bytes, digest_bytes);
}

static int process(void *object)
{
	struct impl *self = object;
	struct spa_io_buffers *io;
	struct spa_buffer *buffer;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	metric_add(&self->metrics.process_calls, 1);
	if (!self->started || (io = self->input.io) == NULL)
		return SPA_STATUS_OK;
	if (io->status != SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_NEED_DATA;
	if (io->buffer_id >= self->input.n_buffers ||
			(buffer = self->input.buffers[io->buffer_id]) == NULL) {
		metric_add(&self->metrics.protocol_errors, 1);
		io->status = -EPROTO;
		return -EPROTO;
	}
	record_buffer(self, buffer);
	io->status = SPA_STATUS_NEED_DATA;
	return SPA_STATUS_NEED_DATA;
}

static const struct spa_node_methods node_methods = {
	.version = SPA_VERSION_NODE_METHODS,
	.add_listener = add_listener,
	.set_callbacks = set_callbacks,
	.enum_params = enum_params,
	.set_param = set_param,
	.set_io = set_io,
	.send_command = send_command,
	.add_port = add_port,
	.remove_port = remove_port,
	.port_enum_params = port_enum_params,
	.port_set_param = port_set_param,
	.port_use_buffers = port_use_buffers,
	.port_set_io = port_set_io,
	.port_reuse_buffer = reuse_buffer,
	.process = process,
};

static int get_interface(struct spa_handle *handle, const char *type,
		void **interface)
{
	struct impl *self = (struct impl *)handle;

	spa_return_val_if_fail(self != NULL && interface != NULL, -EINVAL);
	if (!spa_streq(type, SPA_TYPE_INTERFACE_Node))
		return -ENOENT;
	*interface = &self->node;
	return 0;
}

static int clear(struct spa_handle *handle)
{
	struct impl *self = (struct impl *)handle;

	spa_return_val_if_fail(self != NULL, -EINVAL);
	self->started = false;
	free(self->input.format);
	self->input.format = NULL;
	return 0;
}

static size_t get_size(const struct spa_handle_factory *factory,
		const struct spa_dict *params)
{
	(void)factory;
	(void)params;
	return sizeof(struct impl);
}

static int init(const struct spa_handle_factory *factory,
		struct spa_handle *handle, const struct spa_dict *info,
		const struct spa_support *support, uint32_t n_support)
{
	struct impl *self;
	const char *node_name = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_NODE_NAME);
	const char *node_description = info == NULL ? NULL :
			spa_dict_lookup(info, SPA_KEY_NODE_DESCRIPTION);
	const char *minimum_buffer_size = info == NULL ? NULL :
			spa_dict_lookup(info,
					SPA_KEY_API_PIPEWIREAO_DISCARD_MINIMUM_BUFFER_SIZE);
	uint32_t node_item_count = 0;
	int res;

	(void)factory;
	spa_return_val_if_fail(handle != NULL, -EINVAL);
	self = (struct impl *)handle;
	memset(self, 0, sizeof(*self));
	self->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	self->handle.get_interface = get_interface;
	self->handle.clear = clear;
	spa_hook_list_init(&self->hooks);
	atomic_init(&self->metrics.buffers, 0);
	atomic_init(&self->metrics.data_blocks, 0);
	atomic_init(&self->metrics.bytes, 0);
	atomic_init(&self->metrics.protocol_errors, 0);
	atomic_init(&self->metrics.process_calls, 0);
	atomic_init(&self->metrics.payload_digest, FNV1A_OFFSET_BASIS);
	atomic_init(&self->metrics.digest_bytes, 0);
	if (!atomic_is_lock_free(&self->metrics.buffers) ||
			!atomic_is_lock_free(&self->metrics.data_blocks) ||
			!atomic_is_lock_free(&self->metrics.bytes) ||
			!atomic_is_lock_free(&self->metrics.protocol_errors) ||
			!atomic_is_lock_free(&self->metrics.process_calls) ||
			!atomic_is_lock_free(&self->metrics.payload_digest) ||
			!atomic_is_lock_free(&self->metrics.digest_bytes))
		return -ENOTSUP;
	if ((res = copy_string(self->node_name, sizeof(self->node_name),
				node_name == NULL ? "pipewireao_discard" : node_name)) < 0 ||
			(res = copy_string(self->node_description,
				sizeof(self->node_description),
				node_description == NULL ?
				"PipeWireAO format-agnostic discard sink" :
				node_description)) < 0)
		return res;
	if (minimum_buffer_size != NULL &&
			(!spa_atou32(minimum_buffer_size, &self->minimum_buffer_size, 10) ||
			 self->minimum_buffer_size > INT32_MAX))
		return -EINVAL;
	if (self->minimum_buffer_size != 0 &&
			(res = copy_string(self->minimum_buffer_size_text,
				sizeof(self->minimum_buffer_size_text),
				minimum_buffer_size)) < 0)
		return res;

	self->node.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Node,
			SPA_VERSION_NODE, &node_methods, self);
	self->info_all = SPA_NODE_CHANGE_MASK_FLAGS | SPA_NODE_CHANGE_MASK_PROPS |
			SPA_NODE_CHANGE_MASK_PARAMS;
	self->info = SPA_NODE_INFO_INIT();
	self->info.max_input_ports = 1;
	self->info.flags = SPA_NODE_FLAG_RT;
	self->params[0] = SPA_PARAM_INFO(SPA_PARAM_PropInfo,
			SPA_PARAM_INFO_READ);
	self->params[1] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READ);
	self->params[2] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	self->info.params = self->params;
	self->info.n_params = SPA_N_ELEMENTS(self->params);
	self->node_items[node_item_count++] =
			SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "discard");
	self->node_items[node_item_count++] =
			SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_ROLE, "Test");
	self->node_items[node_item_count++] =
			SPA_DICT_ITEM_INIT(SPA_KEY_NODE_NAME, self->node_name);
	self->node_items[node_item_count++] =
			SPA_DICT_ITEM_INIT(SPA_KEY_NODE_DESCRIPTION,
					self->node_description);
	if (self->minimum_buffer_size != 0)
		self->node_items[node_item_count++] = SPA_DICT_ITEM_INIT(
				SPA_KEY_API_PIPEWIREAO_DISCARD_MINIMUM_BUFFER_SIZE,
				self->minimum_buffer_size_text);
	self->node_props = SPA_DICT_INIT(self->node_items, node_item_count);
	self->info.props = &self->node_props;

	self->input.info_all = SPA_PORT_CHANGE_MASK_FLAGS |
			SPA_PORT_CHANGE_MASK_PROPS | SPA_PORT_CHANGE_MASK_PARAMS;
	self->input.info = SPA_PORT_INFO_INIT();
	self->input.info.flags = SPA_PORT_FLAG_NO_REF | SPA_PORT_FLAG_TERMINAL;
	self->input.prop_items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_PORT_NAME, "in");
	self->input.props = SPA_DICT_INIT(self->input.prop_items,
			SPA_N_ELEMENTS(self->input.prop_items));
	self->input.info.props = &self->input.props;
	self->input.params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat,
			SPA_PARAM_INFO_READ);
	self->input.params[1] = SPA_PARAM_INFO(SPA_PARAM_Format,
			SPA_PARAM_INFO_READWRITE);
	self->input.params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers,
			SPA_PARAM_INFO_READ);
	self->input.params[3] = SPA_PARAM_INFO(SPA_PARAM_IO,
			SPA_PARAM_INFO_READ);
	self->input.info.params = self->input.params;
	self->input.info.n_params = SPA_N_ELEMENTS(self->input.params);

	return 0;
}

static const struct spa_interface_info interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node },
};

static int enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
{
	(void)factory;
	spa_return_val_if_fail(info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(interfaces))
		return 0;
	*info = &interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory spa_pipewireao_discard_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_PIPEWIREAO_DISCARD,
	NULL,
	get_size,
	init,
	enum_interface_info,
};
