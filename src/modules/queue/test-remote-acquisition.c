/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <inttypes.h>
#include <endian.h>

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/node/command.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define PAYLOAD_WORDS 64u
#define SAMPLE_COUNT 64u
#define SAMPLE_STRIDE 16u
#define TIMEOUT_NSEC (5u * SPA_NSEC_PER_SEC)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct test_data;

struct receipt {
	uint64_t sequence;
	uintptr_t buffer_ordinal;
	uint32_t busy_before, busy_after;
};

struct endpoint {
	struct test_data *test;
	struct pw_stream *stream;
	struct spa_hook listener;
	_Atomic int state;
	_Atomic uint32_t errors;
	_Atomic uint32_t requested;
	_Atomic uint32_t publications;
	_Atomic uint32_t deliveries;
	_Atomic uint32_t trigger_done;
	_Atomic uint32_t process_calls;
	uint32_t buffers;
	struct receipt receipts[SAMPLE_COUNT];
};

struct node_watch {
	struct test_data *test;
	struct pw_node *node;
	struct spa_hook listener;
	struct pw_properties *properties;
	uint32_t id;
	bool input;
	bool ports_valid;
};

struct port_identity {
	uint32_t id, node;
	bool input;
};

struct test_data {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_proxy *required_link;
	struct pw_proxy *capture_link;
	struct pw_proxy *playback_link;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	struct port_identity ports[64];
	uint32_t n_ports;
	bool header_only;
	bool pause_source;
	int done_seq;
	bool discovered;
	struct node_watch input;
	struct node_watch output;
	struct endpoint producer;
	struct endpoint observer;
	struct endpoint required;
	char input_queue_id[64];
	char output_queue_id[64];
};

static void maybe_complete(struct test_data *data)
{
	if (data->input.id != 0 && data->output.id != 0 &&
			data->input.ports_valid && data->output.ports_valid &&
			data->input_queue_id[0] != '\0' &&
			data->output_queue_id[0] != '\0')
		data->discovered = true;
}

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static void iterate_main_loop(struct test_data *data)
{
	struct pw_loop *loop = pw_main_loop_get_loop(data->main_loop);

	(void)pw_loop_iterate(loop, 10);
}

static void wait_for_discovery(struct test_data *data)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (!data->discovered && monotonic_nsec() < deadline)
		iterate_main_loop(data);
	CHECK(data->discovered);
}

static void wait_for_state(struct test_data *data, struct endpoint *endpoint,
		int expected)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (atomic_load_explicit(&endpoint->state, memory_order_acquire) !=
			expected && monotonic_nsec() < deadline) {
		CHECK(atomic_load_explicit(&endpoint->state,
				memory_order_relaxed) != PW_STREAM_STATE_ERROR);
		iterate_main_loop(data);
	}
	CHECK(atomic_load_explicit(&endpoint->state,
			memory_order_acquire) == expected);
}

static void on_node_info(void *user_data, const struct pw_node_info *info)
{
	struct node_watch *watch = user_data;
	struct test_data *data = watch->test;
	const char *queue_id;
	char *destination;
	size_t capacity;

	if (watch->input) {
		CHECK(info->n_input_ports == 1 && info->n_output_ports == 0);
	} else {
		CHECK(info->n_input_ports == 0 && info->n_output_ports == 1);
	}
	watch->ports_valid = true;
	if (info->props == NULL) {
		maybe_complete(data);
		return;
	}
	if (watch->properties == NULL)
		watch->properties = pw_properties_new_dict(info->props);
	else
		(void)pw_properties_update(watch->properties, info->props);
	CHECK(watch->properties != NULL);
	queue_id = spa_dict_lookup(info->props, PWAO_QUEUE_ID_PROPERTY);
	if (queue_id == NULL)
		return;
	destination = watch->input ? data->input_queue_id :
			data->output_queue_id;
	capacity = watch->input ? sizeof(data->input_queue_id) :
			sizeof(data->output_queue_id);
	CHECK(strlen(queue_id) < capacity);
	snprintf(destination, capacity, "%s", queue_id);
	maybe_complete(data);
}

static const char *node_diagnostic(const struct node_watch *watch,
		const char *key)
{
	const char *value = watch->properties == NULL ? NULL :
			pw_properties_get(watch->properties, key);

	return value == NULL ? "unavailable" : value;
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = on_node_info,
};

static void on_global(void *user_data, uint32_t id, uint32_t permissions,
		const char *type, uint32_t version, const struct spa_dict *props)
{
	struct test_data *data = user_data;
	struct node_watch *watch;
	const char *name;

	(void)permissions;
	if (props != NULL && spa_streq(type, PW_TYPE_INTERFACE_Port)) {
		const char *node = spa_dict_lookup(props, PW_KEY_NODE_ID);
		const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
		CHECK(node != NULL && direction != NULL && data->n_ports < SPA_N_ELEMENTS(data->ports));
		data->ports[data->n_ports++] = (struct port_identity) {
			.id = id, .node = (uint32_t)strtoul(node, NULL, 10),
			.input = spa_streq(direction, "in"),
		};
		return;
	}
	if (props == NULL || !spa_streq(type, PW_TYPE_INTERFACE_Node))
		return;
	name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
	if (name == NULL)
		return;
	if (spa_streq(name, "test.remote-queue-input"))
		watch = &data->input;
	else if (spa_streq(name, "test.remote-queue-output"))
		watch = &data->output;
	else
		return;
	if (watch->node != NULL)
		return;
	watch->id = id;
	watch->node = pw_registry_bind(data->registry, id,
			PW_TYPE_INTERFACE_Node, SPA_MIN(version, (uint32_t)PW_VERSION_NODE), 0);
	CHECK(watch->node != NULL);
	pw_node_add_listener(watch->node, &watch->listener, &node_events, watch);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
};

static void endpoint_state_changed(void *user_data,
		enum pw_stream_state old, enum pw_stream_state state,
		const char *error)
{
	struct endpoint *endpoint = user_data;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "remote stream error: %s\n",
				error == NULL ? "unknown" : error);
		atomic_fetch_add_explicit(&endpoint->errors, 1,
				memory_order_relaxed);
	}
	atomic_store_explicit(&endpoint->state, state, memory_order_release);
}

static void endpoint_trigger_done(void *user_data)
{
	struct endpoint *endpoint = user_data;

	atomic_fetch_add_explicit(&endpoint->trigger_done, 1,
			memory_order_release);
}

static void producer_process(void *user_data)
{
	struct endpoint *producer = user_data;
	struct pw_buffer *pw_buffer;
	struct spa_buffer *buffer;
	struct spa_meta_header *header;
	uint32_t requested, sequence, i;
	uint16_t *payload;
	struct spa_meta *meta;
	struct spa_meta_acquisition *acquisition;

	atomic_fetch_add_explicit(&producer->process_calls, 1, memory_order_relaxed);
	requested = atomic_load_explicit(&producer->requested,
			memory_order_acquire);
	while (requested != 0 && !atomic_compare_exchange_weak_explicit(
			&producer->requested, &requested, requested - 1u,
			memory_order_acq_rel, memory_order_relaxed))
		;
	if (requested == 0)
		return;
	pw_buffer = pw_stream_dequeue_buffer(producer->stream);
	if (pw_buffer == NULL) {
		atomic_fetch_add_explicit(&producer->requested, 1,
				memory_order_release);
		return;
	}
	buffer = pw_buffer->buffer;
	if (buffer->n_datas != 1 || buffer->datas[0].data == NULL ||
			buffer->datas[0].chunk == NULL ||
			buffer->datas[0].maxsize < PAYLOAD_WORDS * sizeof(uint16_t))
		goto error;
	sequence = atomic_load_explicit(&producer->publications,
			memory_order_relaxed) + 1u;
	if (sequence > SAMPLE_COUNT)
		goto error;
	payload = buffer->datas[0].data;
	for (i = 0; i < PAYLOAD_WORDS; i++)
		payload[i] = htole16((uint16_t)(sequence * 256u + i));
	buffer->datas[0].chunk->offset = 0;
	buffer->datas[0].chunk->size = PAYLOAD_WORDS * sizeof(uint16_t);
	buffer->datas[0].chunk->stride = SAMPLE_STRIDE;
	buffer->datas[0].chunk->flags = 0;
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL)
		goto error;
	header->flags = 0;
	header->offset = 0;
	header->pts = sequence * 2000000;
	header->dts_offset = 0;
	header->seq = sequence;
	if (producer->test->header_only)
		goto publish;
	meta = spa_buffer_find_meta(buffer, SPA_META_Acquisition);
	if (meta == NULL || meta->size != SPA_META_ACQUISITION_SIZE)
		goto error;
	acquisition = meta->data;
	memset(acquisition, 0, sizeof(*acquisition));
	acquisition->version = SPA_META_ACQUISITION_VERSION;
	acquisition->abi_size = SPA_META_ACQUISITION_SIZE;
	acquisition->flags = SPA_META_ACQUISITION_FLAG_IDENTITY_VALID |
		SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID |
		SPA_META_ACQUISITION_FLAG_EXPOSURE_DURATION_VALID;
	acquisition->timebase = SPA_META_ACQUISITION_TIMEBASE_MONOTONIC;
	acquisition->domain[0] = 0xa0;
	acquisition->generation = 1;
	acquisition->sequence = sequence;
	acquisition->exposure_start_nsec = header->pts;
	acquisition->exposure_duration_nsec = 2000000;
publish:
	if (pw_stream_queue_buffer(producer->stream, pw_buffer) < 0)
		atomic_fetch_add_explicit(&producer->errors, 1,
				memory_order_relaxed);
	else
		atomic_store_explicit(&producer->publications, sequence,
				memory_order_release);
	return;

error:
	atomic_fetch_add_explicit(&producer->errors, 1, memory_order_relaxed);
	(void)pw_stream_queue_buffer(producer->stream, pw_buffer);
}

static bool validate_observer_buffer(struct endpoint *observer,
		struct pw_buffer *pw_buffer, uint32_t *sequence)
{
	struct spa_buffer *buffer = pw_buffer->buffer;
	struct spa_meta_header *header;
	uint16_t *payload;
	uint32_t i;
	struct spa_meta *meta;
	struct spa_meta_acquisition *acquisition;

	if (buffer->n_datas != 1 || buffer->datas[0].data == NULL ||
			buffer->datas[0].chunk == NULL ||
			buffer->datas[0].chunk->offset != 0 ||
			buffer->datas[0].chunk->size !=
				PAYLOAD_WORDS * sizeof(uint16_t) ||
			buffer->datas[0].chunk->stride != SAMPLE_STRIDE)
		return false;
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL || header->seq == 0 ||
			header->seq > SAMPLE_COUNT ||
			header->pts != (int64_t)header->seq * 2000000 ||
			header->flags != 0)
		return false;
	*sequence = header->seq;
	payload = buffer->datas[0].data;
	for (i = 0; i < PAYLOAD_WORDS; i++)
		if (le16toh(payload[i]) != *sequence * 256u + i)
			return false;
	if (observer->test->header_only)
		return true;
	meta = spa_buffer_find_meta(buffer, SPA_META_Acquisition);
	if (meta == NULL || meta->size != SPA_META_ACQUISITION_SIZE ||
			!spa_meta_acquisition_is_valid(meta))
		return false;
	acquisition = meta->data;
	if (acquisition->version != SPA_META_ACQUISITION_VERSION ||
			acquisition->generation != 1 ||
			acquisition->sequence != *sequence ||
			acquisition->domain[0] != 0xa0 ||
			acquisition->flags != (SPA_META_ACQUISITION_FLAG_IDENTITY_VALID |
				SPA_META_ACQUISITION_FLAG_EXPOSURE_START_VALID |
				SPA_META_ACQUISITION_FLAG_EXPOSURE_DURATION_VALID) ||
			acquisition->timebase != SPA_META_ACQUISITION_TIMEBASE_MONOTONIC ||
			acquisition->exposure_start_nsec != header->pts ||
			acquisition->exposure_duration_nsec != 2000000)
		return false;
	for (i = 1; i < SPA_META_ACQUISITION_DOMAIN_SIZE; i++)
		if (acquisition->domain[i] != 0)
			return false;
	return true;
}

static void observer_process(void *user_data)
{
	struct endpoint *observer = user_data;
	struct pw_buffer *buffer;

	atomic_fetch_add_explicit(&observer->process_calls, 1, memory_order_relaxed);
	while ((buffer = pw_stream_dequeue_buffer(observer->stream)) != NULL) {
		uint32_t sequence, index = atomic_load_explicit(&observer->deliveries,
				memory_order_relaxed);
		struct spa_meta_busy *busy = spa_buffer_find_meta_data(buffer->buffer,
				SPA_META_Busy, sizeof(*busy));
		uint32_t before = busy == NULL ? UINT32_MAX :
			__atomic_load_n(&busy->count, __ATOMIC_RELAXED);
		if (index >= SAMPLE_COUNT ||
				!validate_observer_buffer(observer, buffer, &sequence) ||
				sequence != index + 1u) {
			atomic_fetch_add_explicit(&observer->errors, 1, memory_order_relaxed);
			(void)pw_stream_queue_buffer(observer->stream, buffer);
			continue;
		}
		observer->receipts[index].sequence = sequence;
		observer->receipts[index].buffer_ordinal = (uintptr_t)buffer->user_data;
		observer->receipts[index].busy_before = before;
		if (pw_stream_queue_buffer(observer->stream, buffer) < 0) {
			atomic_fetch_add_explicit(&observer->errors, 1, memory_order_relaxed);
			continue;
		}
		observer->receipts[index].busy_after = busy == NULL ? UINT32_MAX :
			__atomic_load_n(&busy->count, __ATOMIC_RELAXED);
		atomic_store_explicit(&observer->deliveries, index + 1u,
				memory_order_release);
	}
}

static void endpoint_add_buffer(void *user_data, struct pw_buffer *buffer)
{
	struct endpoint *endpoint = user_data;
	buffer->user_data = (void *)(uintptr_t)++endpoint->buffers;
	printf("buffer-added endpoint=%s ordinal=%u bytes=%u", pw_stream_get_name(endpoint->stream),
		endpoint->buffers, buffer->buffer->n_datas ? buffer->buffer->datas[0].maxsize : 0);
	for (uint32_t i = 0; i < buffer->buffer->n_metas; i++)
		printf(" meta=%u/%u", buffer->buffer->metas[i].type, buffer->buffer->metas[i].size);
	putchar('\n');
}

static void endpoint_param_changed(void *user_data, uint32_t id,
		const struct spa_pod *param);

static const struct pw_stream_events producer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = endpoint_state_changed,
	.process = producer_process,
	.add_buffer = endpoint_add_buffer,
	.param_changed = endpoint_param_changed,
	.trigger_done = endpoint_trigger_done,
};

static const struct pw_stream_events observer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = endpoint_state_changed,
	.process = observer_process,
	.add_buffer = endpoint_add_buffer,
	.param_changed = endpoint_param_changed,
	.trigger_done = endpoint_trigger_done,
};

static struct spa_pod *build_format(uint8_t *storage, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { 8, 8 };
	const struct spa_fraction rate = SPA_FRACTION(500, 1);

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String("org.pipewireao.test.queue.remote/1"),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_U16_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
				SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate));
}

static void endpoint_param_changed(void *user_data, uint32_t id,
		const struct spa_pod *param)
{
	struct endpoint *endpoint = user_data;
	uint8_t format_storage[1024], buffers_storage[256], header_storage[256],
		acquisition_storage[256];
	struct spa_pod_builder acquisition_builder = SPA_POD_BUILDER_INIT(
			acquisition_storage, sizeof(acquisition_storage));
	struct spa_pod_frame frame;
	struct spa_pod_builder buffers_builder = SPA_POD_BUILDER_INIT(
			buffers_storage, sizeof(buffers_storage));
	struct spa_pod_builder header_builder = SPA_POD_BUILDER_INIT(
			header_storage, sizeof(header_storage));
	const struct spa_pod *params[4];
	if (id != SPA_PARAM_Format || param == NULL)
		return;
	params[0] = build_format(format_storage, sizeof(format_storage));
	params[1] = spa_pod_builder_add_object(&buffers_builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(3),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,
			SPA_POD_Int(PAYLOAD_WORDS * (int32_t)sizeof(uint16_t)),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int(SAMPLE_STRIDE),
			SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_MemFd));
	params[2] = spa_pod_builder_add_object(&header_builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int((int32_t)sizeof(struct spa_meta_header)));
	spa_pod_builder_push_object(&acquisition_builder, &frame,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
	spa_pod_builder_add(&acquisition_builder,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
			SPA_PARAM_META_size, SPA_POD_Int(SPA_META_ACQUISITION_SIZE), 0);
	spa_pod_builder_prop(&acquisition_builder, SPA_PARAM_META_features,
			SPA_POD_PROP_FLAG_MANDATORY);
	spa_pod_builder_int(&acquisition_builder, SPA_META_FEATURE_ACQUISITION_CURRENT);
	params[3] = spa_pod_builder_pop(&acquisition_builder, &frame);
	CHECK(params[0] != NULL && params[1] != NULL && params[2] != NULL &&
			params[3] != NULL);
	CHECK(pw_stream_update_params(endpoint->stream, params + 1, endpoint->test->header_only ? 2 : 3) == 0);
}

static void create_endpoint(struct test_data *data, struct endpoint *endpoint,
		const char *name, enum pw_direction direction,
		const struct pw_stream_events *events)
{
	uint8_t format_storage[1024];
	const struct spa_pod *params[1];

	endpoint->test = data;
	atomic_init(&endpoint->state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&endpoint->errors, 0);
	atomic_init(&endpoint->requested, 0);
	atomic_init(&endpoint->publications, 0);
	atomic_init(&endpoint->deliveries, 0);
	atomic_init(&endpoint->trigger_done, 0);
	atomic_init(&endpoint->process_calls, 0);
	endpoint->stream = pw_stream_new(data->core, name,
			pw_properties_new(PW_KEY_NODE_NAME, name,
				PW_KEY_NODE_VIRTUAL, "true",
				PW_KEY_NODE_PAUSE_ON_IDLE, "false", NULL));
	CHECK(endpoint->stream != NULL);
	pw_stream_add_listener(endpoint->stream, &endpoint->listener,
			events, endpoint);
	params[0] = build_format(format_storage, sizeof(format_storage));
	CHECK(pw_stream_connect(endpoint->stream, direction, PW_ID_ANY,
			(strcmp(name, "test.remote-queue-required") == 0 ? 0 : PW_STREAM_FLAG_DRIVER) |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT |
			PW_STREAM_FLAG_RT_TRIGGER_DONE,
			params, SPA_N_ELEMENTS(params)) == 0);
}

static struct pw_proxy *create_link(struct test_data *data,
		uint32_t output_node, uint32_t input_node)
{
	struct pw_properties *properties = pw_properties_new(NULL, NULL);
	struct pw_proxy *link;

	CHECK(properties != NULL);
	uint32_t output_port = PW_ID_ANY, input_port = PW_ID_ANY;
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;
	while ((output_port == PW_ID_ANY || input_port == PW_ID_ANY) &&
			monotonic_nsec() < deadline) {
		for (uint32_t i = 0; i < data->n_ports; i++) {
			struct port_identity *port = &data->ports[i];
			if (port->node == output_node && !port->input)
				output_port = port->id;
			if (port->node == input_node && port->input)
				input_port = port->id;
		}
		if (output_port == PW_ID_ANY || input_port == PW_ID_ANY)
			iterate_main_loop(data);
	}
	CHECK(output_port != PW_ID_ANY && input_port != PW_ID_ANY);
	CHECK(pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_PORT, "%u", output_port) >= 0);
	CHECK(pw_properties_setf(properties, PW_KEY_LINK_INPUT_PORT, "%u", input_port) >= 0);
	CHECK(pw_properties_set(properties, PW_KEY_OBJECT_LINGER, "false") >= 0);
	if (input_node == data->input.id)
		CHECK(pw_properties_set(properties, PW_KEY_LINK_PASSIVE, "true") >= 0);
	printf("link output=%u/%u input=%u/%u passive=%s\n", output_node, output_port,
		input_node, input_port, input_node == data->input.id ? "true" : "false");
	CHECK(pw_properties_setf(properties, PW_KEY_LINK_OUTPUT_NODE, "%u",
			output_node) >= 0);
	CHECK(pw_properties_setf(properties, PW_KEY_LINK_INPUT_NODE, "%u",
			input_node) >= 0);
	link = pw_core_create_object(data->core, "link-factory",
			PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
			&properties->dict, 0);
	pw_properties_free(properties);
	CHECK(link != NULL);
	return link;
}

static void on_core_error(void *user_data, uint32_t id, int seq, int result,
		const char *message)
{
	(void)user_data;
	(void)id;
	(void)seq;
	fprintf(stderr, "remote core error %d: %s\n", result,
			message == NULL ? "unknown" : message);
	abort();
}

static void on_core_done(void *user_data, uint32_t id, int seq)
{
	struct test_data *data = user_data;

	(void)id;
	data->done_seq = seq;
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_core_done,
	.error = on_core_error,
};

static void sync_core(struct test_data *data)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;
	int seq = pw_core_sync(data->core, PW_ID_CORE, 0);

	CHECK(seq >= 0);
	while (data->done_seq != seq && monotonic_nsec() < deadline)
		iterate_main_loop(data);
	CHECK(data->done_seq == seq);
}

static uint32_t wait_for_node_id(struct test_data *data,
		struct endpoint *endpoint)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;
	uint32_t id;

	while ((id = pw_stream_get_node_id(endpoint->stream)) == SPA_ID_INVALID &&
			monotonic_nsec() < deadline)
		iterate_main_loop(data);
	CHECK(id != SPA_ID_INVALID);
	return id;
}

static void diagnostics(struct test_data *data)
{
	static const char *keys[] = {
		"object.serial", "node.driver-id", "node.loop.name", "pipewireao.queue.id",
		"queue.state.capture-generation", "queue.state.requested-generation",
		"queue.state.installed-generation", "queue.state.configured",
		"queue.state.capture-buffers", "queue.state.playback-buffers",
		"queue.state.active-outputs", "queue.state.pending-depth",
		"queue.state.completion-depth", "queue.stats.publications",
		"queue.stats.deliveries", "queue.stats.completions",
		"queue.stats.replacements", "queue.stats.protocol-errors",
		"queue.stats.pool-exhaustions", "queue.state.output-generation",
		"queue.state.input-stream", "queue.state.output-stream",
		"queue.error.operation",
	};
	struct node_watch *nodes[] = { &data->input, &data->output };
	for (uint32_t n = 0; n < SPA_N_ELEMENTS(nodes); n++) {
		printf("queue side=%s id=%u", n == 0 ? "capture" : "playback", nodes[n]->id);
		for (uint32_t i = 0; i < SPA_N_ELEMENTS(keys); i++)
			printf(" %s=%s", keys[i], node_diagnostic(nodes[n], keys[i]));
		putchar('\n');
	}
	printf("client publications=%u deliveries=%u required-deliveries=%u trigger-done=%u/%u errors=%u/%u/%u\n",
		atomic_load(&data->producer.publications), atomic_load(&data->observer.deliveries),
		atomic_load(&data->required.deliveries),
		atomic_load(&data->producer.trigger_done), atomic_load(&data->observer.trigger_done),
		atomic_load(&data->producer.errors), atomic_load(&data->observer.errors),
		atomic_load(&data->required.errors));
	printf("callbacks producer=%u required=%u observer=%u\n",
		atomic_load(&data->producer.process_calls), atomic_load(&data->required.process_calls),
		atomic_load(&data->observer.process_calls));
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct test_data data = { 0 };
	uint32_t producer_id, observer_id, required_id, flushed = 0;
	uint64_t start, deadline, next_publication, next_diagnostic;
	bool passed, paused = false;

	CHECK(argc >= 2 && argc <= 4);
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--header-only") == 0)
			data.header_only = true;
		else if (strcmp(argv[i], "--pause") == 0)
			data.pause_source = true;
		else
			CHECK(false);
	}
	pw_init(&argc, &argv);
	data.main_loop = pw_main_loop_new(NULL);
	CHECK(data.main_loop != NULL);
	data.context = pw_context_new(pw_main_loop_get_loop(data.main_loop), NULL, 0);
	CHECK(data.context != NULL);
	data.core = pw_context_connect(data.context,
		pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL), 0);
	CHECK(data.core != NULL);
	data.input.test = &data;
	data.input.input = true;
	data.output.test = &data;
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
	CHECK(data.registry != NULL);
	pw_registry_add_listener(data.registry, &data.registry_listener, &registry_events, &data);
	create_endpoint(&data, &data.producer, "test.remote-queue-producer",
			PW_DIRECTION_OUTPUT, &producer_events);
	create_endpoint(&data, &data.required, "test.remote-queue-required",
			PW_DIRECTION_INPUT, &observer_events);
	create_endpoint(&data, &data.observer, "test.remote-queue-observer",
			PW_DIRECTION_INPUT, &observer_events);
	pw_loop_enter(pw_main_loop_get_loop(data.main_loop));
	wait_for_discovery(&data);
	CHECK(data.input.id != 0 && data.output.id != 0);
	CHECK(data.input.id != data.output.id);
	CHECK(strcmp(data.input_queue_id, data.output_queue_id) == 0);
	producer_id = wait_for_node_id(&data, &data.producer);
	observer_id = wait_for_node_id(&data, &data.observer);
	required_id = wait_for_node_id(&data, &data.required);
	/* The required consumer establishes the source's Acquisition pool before
	 * optional fan-out, matching RTC admission order. */
	data.required_link = create_link(&data, producer_id, required_id);
	sync_core(&data);
	wait_for_state(&data, &data.required, PW_STREAM_STATE_STREAMING);
	wait_for_state(&data, &data.producer, PW_STREAM_STATE_STREAMING);
	data.capture_link = create_link(&data, producer_id, data.input.id);
	data.playback_link = create_link(&data, data.output.id, observer_id);
	sync_core(&data);
	wait_for_state(&data, &data.producer, PW_STREAM_STATE_STREAMING);
	wait_for_state(&data, &data.observer, PW_STREAM_STATE_STREAMING);
	printf("ready producer=%u required=%u capture=%u playback=%u observer=%u acquisition-size=%zu\n",
		producer_id, required_id, data.input.id, data.output.id, observer_id,
		sizeof(struct spa_meta_acquisition));
	start = monotonic_nsec();
	deadline = start + 12u * SPA_NSEC_PER_SEC;
	next_publication = next_diagnostic = start;
	while (monotonic_nsec() < deadline) {
		uint64_t now = monotonic_nsec();
		uint32_t publications = atomic_load_explicit(&data.producer.publications,
				memory_order_acquire);
		if (data.pause_source && !paused && publications == SAMPLE_COUNT / 2u &&
				atomic_load_explicit(&data.observer.deliveries, memory_order_acquire) == publications) {
			/* No producer trigger is issued during this ingress pause. */
			struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
			CHECK(pw_node_send_command(data.input.node, &pause) >= 0);
			sync_core(&data);
			uint64_t pause_until = monotonic_nsec() + 2u * SPA_NSEC_PER_SEC;
			while (monotonic_nsec() < pause_until) {
				CHECK(pw_stream_trigger_process(data.observer.stream) >= 0);
				iterate_main_loop(&data);
			}
			diagnostics(&data);
			CHECK(strcmp(node_diagnostic(&data.input, "queue.state.input-stream"), "paused") == 0);
			CHECK(atomic_load(&data.required.deliveries) == publications);
			printf("queue-pause sequence=%u input-stream=paused required-preserved=true\n", publications);
			struct spa_command resume = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
			CHECK(pw_node_send_command(data.input.node, &resume) >= 0);
			sync_core(&data);
			wait_for_state(&data, &data.producer, PW_STREAM_STATE_STREAMING);
			paused = true;
			now = monotonic_nsec();
			next_publication = now;
		}
		if (now >= next_publication && publications < SAMPLE_COUNT &&
				atomic_load(&data.producer.requested) == 0) {
			atomic_fetch_add_explicit(&data.producer.requested, 1, memory_order_release);
			next_publication = now + SPA_NSEC_PER_SEC / 10u;
		}
		CHECK(pw_stream_trigger_process(data.producer.stream) >= 0);
		CHECK(pw_stream_trigger_process(data.observer.stream) >= 0);
		iterate_main_loop(&data);
		uint32_t count = atomic_load_explicit(&data.observer.deliveries, memory_order_acquire);
		for (; flushed < count; flushed++) {
			struct receipt *r = &data.observer.receipts[flushed];
			printf("receipt sequence=%" PRIu64 " acquisition=%s "
				"buffer-ordinal=%" PRIuPTR " busy-before=%u busy-after=%u payload=exact\n",
				r->sequence, data.header_only ? "absent" : "a0000000000000000000000000000000/1",
				r->buffer_ordinal, r->busy_before, r->busy_after);
		}
		if (now >= next_diagnostic) {
			diagnostics(&data);
			next_diagnostic = now + SPA_NSEC_PER_SEC;
		}
		if (publications == SAMPLE_COUNT && count == SAMPLE_COUNT &&
				now > next_publication + SPA_NSEC_PER_SEC)
			break;
	}
	diagnostics(&data);
	passed = (!data.pause_source || paused) &&
		atomic_load(&data.producer.publications) == SAMPLE_COUNT &&
		atomic_load(&data.observer.deliveries) == SAMPLE_COUNT &&
		atomic_load(&data.required.deliveries) == SAMPLE_COUNT &&
		atomic_load(&data.required.errors) == 0 &&
		atomic_load(&data.producer.errors) == 0 && atomic_load(&data.observer.errors) == 0;
	pw_proxy_destroy(data.capture_link);
	pw_proxy_destroy(data.playback_link);
	pw_proxy_destroy(data.required_link);
	pw_stream_destroy(data.required.stream);
	pw_stream_destroy(data.observer.stream);
	pw_stream_destroy(data.producer.stream);
	spa_hook_remove(&data.input.listener);
	spa_hook_remove(&data.output.listener);
	pw_properties_free(data.input.properties);
	pw_properties_free(data.output.properties);
	pw_proxy_destroy((struct pw_proxy *)data.input.node);
	pw_proxy_destroy((struct pw_proxy *)data.output.node);
	pw_proxy_destroy((struct pw_proxy *)data.registry);
	pw_core_disconnect(data.core);
	pw_loop_leave(pw_main_loop_get_loop(data.main_loop));
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.main_loop);
	pw_deinit();
	printf("result=%s\n", passed ? "pass" : "fail");
	return passed ? 0 : 1;
}
