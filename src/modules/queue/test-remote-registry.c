/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <pipewire/impl.h>

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define PAYLOAD_WORDS 16u
#define TIMEOUT_NSEC (5u * SPA_NSEC_PER_SEC)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct test_data;

struct file_identity {
	dev_t device;
	ino_t inode;
	_Atomic bool valid;
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
	_Atomic uint32_t removals;
	_Atomic uint32_t trigger_done;
	struct pw_buffer *held;
	uint32_t held_sequence;
	struct file_identity identity[3];
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

struct test_data {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_registry *registry;
	struct pw_impl_module *module;
	struct pw_proxy *capture_link;
	struct pw_proxy *playback_link;
	struct spa_hook core_listener;
	struct spa_hook registry_listener;
	int done_seq;
	bool discovered;
	struct node_watch input;
	struct node_watch output;
	struct endpoint producer;
	struct endpoint observer;
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

static void wait_for_counter(struct test_data *data, _Atomic uint32_t *value,
		uint32_t expected)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (atomic_load_explicit(value, memory_order_acquire) < expected &&
			monotonic_nsec() < deadline)
		iterate_main_loop(data);
	CHECK(atomic_load_explicit(value, memory_order_acquire) >= expected);
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

static void check_presentation(const struct test_data *data)
{
	const struct node_watch *endpoints[] = { &data->input, &data->output };
	const char *group_id = NULL;

	for (uint32_t i = 0; i < SPA_N_ELEMENTS(endpoints); i++) {
		const struct pw_properties *props = endpoints[i]->properties;
		const char *value;

		CHECK(props != NULL);
		value = pw_properties_get(props, "pipewireao.graph.group.id");
		CHECK(value != NULL && value[0] != '\0');
		if (group_id == NULL)
			group_id = value;
		else
			CHECK(strcmp(value, group_id) == 0);
		value = pw_properties_get(props, "pipewireao.graph.group.name");
		CHECK(value != NULL && strcmp(value, "Observer / Queue [exact]-123-456") == 0);
		value = pw_properties_get(props, "pipewireao.graph.group.class");
		CHECK(value != NULL && strcmp(value, "Queue") == 0);
		value = pw_properties_get(props, PW_KEY_NODE_DESCRIPTION);
		CHECK(value != NULL && strcmp(value, i == 0 ?
				"Exact input / [123-456]" : "test.remote-queue-output") == 0);
		value = pw_properties_get(props, PW_KEY_MEDIA_ROLE);
		CHECK(i == 0 ? value != NULL && strcmp(value, "Production") == 0 :
				value == NULL);
	}
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
			PW_TYPE_INTERFACE_Node, SPA_MIN(version, PW_VERSION_NODE), 0);
	CHECK(watch->node != NULL);
	pw_node_add_listener(watch->node, &watch->listener, &node_events, watch);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = on_global,
};

static bool get_file_identity(const struct spa_data *data,
		struct file_identity *identity)
{
	struct stat status;

	if (data->fd < 0 || fstat(data->fd, &status) < 0)
		return false;
	identity->device = status.st_dev;
	identity->inode = status.st_ino;
	atomic_store_explicit(&identity->valid, true, memory_order_release);
	return true;
}

static bool same_identity(const struct file_identity *left,
		const struct file_identity *right)
{
	return atomic_load_explicit(&left->valid, memory_order_acquire) &&
			atomic_load_explicit(&right->valid, memory_order_acquire) &&
			left->device == right->device &&
			left->inode == right->inode;
}

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
	uint32_t requested, sequence, *payload, i;

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
			buffer->datas[0].maxsize < PAYLOAD_WORDS * sizeof(uint32_t))
		goto error;
	sequence = atomic_load_explicit(&producer->publications,
			memory_order_relaxed) + 1u;
	CHECK(sequence < SPA_N_ELEMENTS(producer->identity));
	payload = buffer->datas[0].data;
	for (i = 0; i < PAYLOAD_WORDS; i++)
		payload[i] = sequence * 1000u + i;
	buffer->datas[0].chunk->offset = 0;
	buffer->datas[0].chunk->size = PAYLOAD_WORDS * sizeof(uint32_t);
	buffer->datas[0].chunk->stride = sizeof(uint32_t);
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL)
		goto error;
	header->flags = 0;
	header->offset = 0;
	header->pts = sequence;
	header->dts_offset = 0;
	header->seq = sequence;
	CHECK(get_file_identity(&buffer->datas[0],
			&producer->identity[sequence]));
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
	uint32_t *payload, i;

	if (buffer->n_datas != 1 || buffer->datas[0].data == NULL ||
			buffer->datas[0].chunk == NULL ||
			buffer->datas[0].chunk->offset != 0 ||
			buffer->datas[0].chunk->size !=
				PAYLOAD_WORDS * sizeof(uint32_t) ||
			buffer->datas[0].chunk->stride != (int32_t)sizeof(uint32_t))
		return false;
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL || header->seq == 0 ||
			header->seq >= SPA_N_ELEMENTS(observer->identity) ||
			header->pts != (int64_t)header->seq)
		return false;
	*sequence = header->seq;
	payload = buffer->datas[0].data;
	for (i = 0; i < PAYLOAD_WORDS; i++)
		if (payload[i] != *sequence * 1000u + i)
			return false;
	return get_file_identity(&buffer->datas[0],
			&observer->identity[*sequence]);
}

static void observer_process(void *user_data)
{
	struct endpoint *observer = user_data;
	struct endpoint *producer = &observer->test->producer;
	struct pw_buffer *pw_buffer;

	while ((pw_buffer = pw_stream_dequeue_buffer(observer->stream)) != NULL) {
		uint32_t sequence = 0;

		if (!validate_observer_buffer(observer, pw_buffer, &sequence) ||
				!same_identity(&observer->identity[sequence],
					&producer->identity[sequence])) {
			atomic_fetch_add_explicit(&observer->errors, 1,
					memory_order_relaxed);
			(void)pw_stream_queue_buffer(observer->stream, pw_buffer);
			continue;
		}
		if (observer->held == NULL) {
			observer->held = pw_buffer;
			observer->held_sequence = sequence;
			atomic_fetch_add_explicit(&observer->deliveries, 1,
					memory_order_release);
			continue;
		}
		if (pw_stream_queue_buffer(observer->stream, pw_buffer) < 0)
			atomic_fetch_add_explicit(&observer->errors, 1,
					memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&observer->deliveries, 1,
					memory_order_release);
	}
}

static void observer_remove_buffer(void *user_data, struct pw_buffer *buffer)
{
	struct endpoint *observer = user_data;

	if (observer->held == buffer) {
		observer->held = NULL;
		observer->held_sequence = 0;
	}
	atomic_fetch_add_explicit(&observer->removals, 1,
			memory_order_release);
}

static const struct pw_stream_events producer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = endpoint_state_changed,
	.process = producer_process,
	.trigger_done = endpoint_trigger_done,
};

static const struct pw_stream_events observer_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = endpoint_state_changed,
	.process = observer_process,
	.remove_buffer = observer_remove_buffer,
	.trigger_done = endpoint_trigger_done,
};

static struct spa_pod *build_format(uint8_t *storage, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { (int32_t)PAYLOAD_WORDS };
	const struct spa_fraction rate = SPA_FRACTION(1000, 1);

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String("org.pipewireao.test.queue.remote/1"),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_U32_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
				SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate));
}

static void create_endpoint(struct test_data *data, struct endpoint *endpoint,
		const char *name, enum pw_direction direction,
		const struct pw_stream_events *events)
{
	uint8_t format_storage[1024], buffers_storage[256], header_storage[256];
	struct spa_pod_builder buffers_builder = SPA_POD_BUILDER_INIT(
			buffers_storage, sizeof(buffers_storage));
	struct spa_pod_builder header_builder = SPA_POD_BUILDER_INIT(
			header_storage, sizeof(header_storage));
	const struct spa_pod *params[3];

	endpoint->test = data;
	atomic_init(&endpoint->state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&endpoint->errors, 0);
	atomic_init(&endpoint->requested, 0);
	atomic_init(&endpoint->publications, 0);
	atomic_init(&endpoint->deliveries, 0);
	atomic_init(&endpoint->removals, 0);
	atomic_init(&endpoint->trigger_done, 0);
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(endpoint->identity); i++)
		atomic_init(&endpoint->identity[i].valid, false);
	endpoint->stream = pw_stream_new(data->core, name,
			pw_properties_new(PW_KEY_NODE_NAME, name,
				PW_KEY_NODE_VIRTUAL, "true",
				PW_KEY_NODE_PAUSE_ON_IDLE, "false", NULL));
	CHECK(endpoint->stream != NULL);
	pw_stream_add_listener(endpoint->stream, &endpoint->listener,
			events, endpoint);
	params[0] = build_format(format_storage, sizeof(format_storage));
	params[1] = spa_pod_builder_add_object(&buffers_builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(3),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,
			SPA_POD_Int(PAYLOAD_WORDS * (int32_t)sizeof(uint32_t)),
			SPA_PARAM_BUFFERS_stride, SPA_POD_Int(sizeof(uint32_t)),
			SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_MemFd));
	params[2] = spa_pod_builder_add_object(&header_builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int((int32_t)sizeof(struct spa_meta_header)));
	CHECK(params[0] != NULL && params[1] != NULL && params[2] != NULL);
	CHECK(pw_stream_connect(endpoint->stream, direction, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
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

static void drive_graph_cycle(struct test_data *data, struct endpoint *endpoint)
{
	uint32_t completed = atomic_load_explicit(&endpoint->trigger_done,
			memory_order_acquire);

	CHECK(pw_stream_trigger_process(endpoint->stream) >= 0);
	/* A graph rebuilt after pool replacement can accept a trigger before the
	 * new driver activation reaches its data loop.  Bound each wait so the
	 * caller can issue another trigger until its overall deadline. */
	for (uint32_t iteration = 0; iteration < 64u &&
			atomic_load_explicit(&endpoint->trigger_done,
				memory_order_acquire) == completed; iteration++)
		iterate_main_loop(data);
}

static void trigger_producer(struct test_data *data, uint32_t expected)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;
	struct endpoint *producer = &data->producer;

	atomic_fetch_add_explicit(&producer->requested, 1, memory_order_release);
	while (atomic_load_explicit(&producer->publications,
			memory_order_acquire) < expected && monotonic_nsec() < deadline) {
		drive_graph_cycle(data, producer);
	}
	if (atomic_load_explicit(&producer->publications,
			memory_order_acquire) < expected)
		fprintf(stderr, "remote producer stalled: expected=%u publications=%u "
				"requested=%u state=%d errors=%u\n", expected,
				atomic_load_explicit(&producer->publications,
					memory_order_relaxed),
				atomic_load_explicit(&producer->requested,
					memory_order_relaxed),
				atomic_load_explicit(&producer->state, memory_order_relaxed),
				atomic_load_explicit(&producer->errors,
					memory_order_relaxed));
	CHECK(atomic_load_explicit(&producer->publications,
			memory_order_acquire) >= expected);
	CHECK(atomic_load_explicit(&producer->errors,
			memory_order_relaxed) == 0);
}

static void trigger_observer(struct test_data *data, uint32_t expected)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;
	struct endpoint *observer = &data->observer;

	while (atomic_load_explicit(&observer->deliveries,
			memory_order_acquire) < expected && monotonic_nsec() < deadline) {
		/* The synthetic producer is a manually triggered driver.  After graph
		 * relinking, queuing its payload and processing the queue input can require
		 * separate graph cycles.  Keep both independent graphs moving while the
		 * observer polls, as a live camera driver would. */
		drive_graph_cycle(data, &data->producer);
		drive_graph_cycle(data, observer);
	}
	if (atomic_load_explicit(&observer->deliveries,
			memory_order_acquire) < expected)
		fprintf(stderr, "remote observer stalled: expected=%u deliveries=%u "
				"publications=%u removals=%u triggers=%u/%u state=%d errors=%u "
				"ownership=%s output-generation=%s generations=%s/%s/%s "
				"configured=%s buffers=%s/%s depths=%s/%s active=%s "
				"queue-stats=%s/%s/%s protocol-errors=%s operation=%s\n",
				expected,
				atomic_load_explicit(&observer->deliveries,
					memory_order_relaxed),
				atomic_load_explicit(&data->producer.publications,
					memory_order_relaxed),
				atomic_load_explicit(&observer->removals,
					memory_order_relaxed),
				atomic_load_explicit(&data->producer.trigger_done,
					memory_order_relaxed),
				atomic_load_explicit(&observer->trigger_done,
					memory_order_relaxed),
				atomic_load_explicit(&observer->state, memory_order_relaxed),
				atomic_load_explicit(&observer->errors,
					memory_order_relaxed),
				node_diagnostic(&data->output,
					"queue.state.ownership-transition"),
				node_diagnostic(&data->output,
					"queue.state.output-generation"),
				node_diagnostic(&data->output,
					"queue.state.capture-generation"),
				node_diagnostic(&data->output,
					"queue.state.requested-generation"),
				node_diagnostic(&data->output,
					"queue.state.installed-generation"),
				node_diagnostic(&data->output, "queue.state.configured"),
				node_diagnostic(&data->output,
					"queue.state.capture-buffers"),
				node_diagnostic(&data->output,
					"queue.state.playback-buffers"),
				node_diagnostic(&data->output, "queue.state.pending-depth"),
				node_diagnostic(&data->output,
					"queue.state.completion-depth"),
				node_diagnostic(&data->output, "queue.state.active-outputs"),
				node_diagnostic(&data->output, "queue.stats.publications"),
				node_diagnostic(&data->output, "queue.stats.deliveries"),
				node_diagnostic(&data->output,
					"queue.stats.pool-exhaustions"),
				node_diagnostic(&data->output, "queue.stats.protocol-errors"),
				node_diagnostic(&data->output, "queue.error.operation"));
	CHECK(atomic_load_explicit(&observer->deliveries,
			memory_order_acquire) >= expected);
	CHECK(atomic_load_explicit(&observer->errors,
			memory_order_relaxed) == 0);
}

static void release_observer(struct test_data *data)
{
	struct endpoint *observer = &data->observer;
	uint32_t sequence = 0;

	CHECK(observer->held != NULL);
	CHECK(validate_observer_buffer(observer, observer->held, &sequence));
	CHECK(sequence == observer->held_sequence);
	CHECK(pw_stream_queue_buffer(observer->stream, observer->held) == 0);
	observer->held = NULL;
	observer->held_sequence = 0;
}

int main(int argc, char **argv)
{
	struct test_data data = { 0 };
	struct pw_properties *properties;
	char module_args[1024];
	uint32_t producer_id, observer_id, removals, sequence = 0;
	bool serve;
	int length;

	serve = argc == 3 && strcmp(argv[2], "--serve-rtc-observer") == 0;
	CHECK(argc == 2 || serve);
	pw_init(&argc, &argv);
	data.main_loop = pw_main_loop_new(NULL);
	CHECK(data.main_loop != NULL);
	data.context = pw_context_new(pw_main_loop_get_loop(data.main_loop),
			NULL, 0);
	CHECK(data.context != NULL);
	properties = pw_properties_new(PW_KEY_REMOTE_NAME, argv[1], NULL);
	CHECK(properties != NULL);
	data.core = pw_context_connect(data.context, properties, 0);
	CHECK(data.core != NULL);
	data.input.test = &data;
	data.input.input = true;
	data.output.test = &data;
	length = snprintf(module_args, sizeof(module_args),
			"remote.name=%s queue.max-buffers=1 "
			"queue.overflow=drop-oldest queue.storage=%s "
			"queue.media=application/ndarray %s "
			"capture.props={ node.name=%s %s } "
			"playback.props={ node.name=%s }",
			argv[1], serve ? "copy" : "lease",
			serve ? "" : "node.name=ignored-name "
				"node.description=\"Observer / Queue [exact]-123-456\"",
			serve ? "pipewireao-rtc-observation-input" :
				"test.remote-queue-input",
			serve ? "" : "node.description=\"Exact input / [123-456]\" "
				"media.role=Production",
			serve ? "pipewireao-rtc-observation-output" :
				"test.remote-queue-output");
	CHECK(length >= 0 && (size_t)length < sizeof(module_args));
	data.module = pw_context_load_module(data.context,
			"libpipewire-module-queue", module_args, NULL);
	CHECK(data.module != NULL);
	pw_core_add_listener(data.core, &data.core_listener, &core_events, &data);
	data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);
	CHECK(data.registry != NULL);
	pw_registry_add_listener(data.registry, &data.registry_listener,
			&registry_events, &data);
	if (serve) {
		pw_main_loop_run(data.main_loop);
		return 0;
	}
	create_endpoint(&data, &data.producer, "test.remote-queue-producer",
			PW_DIRECTION_OUTPUT, &producer_events);
	create_endpoint(&data, &data.observer, "test.remote-queue-observer",
			PW_DIRECTION_INPUT, &observer_events);
	/* Match pw_main_loop_run(): the loop remains entered for the complete
	 * dispatch interval.  Entering and leaving around each iteration creates a
	 * window where a data-loop invoke treats the main loop as unowned and can
	 * invert the main/data-loop lock order. */
	pw_loop_enter(pw_main_loop_get_loop(data.main_loop));
	wait_for_discovery(&data);
	CHECK(data.input.id != 0 && data.output.id != 0);
	CHECK(data.input.id != data.output.id);
	CHECK(data.input_queue_id[0] != '\0');
	CHECK(strcmp(data.input_queue_id, data.output_queue_id) == 0);
	check_presentation(&data);
	producer_id = wait_for_node_id(&data, &data.producer);
	observer_id = wait_for_node_id(&data, &data.observer);
	data.capture_link = create_link(&data, producer_id, data.input.id);
	data.playback_link = create_link(&data, data.output.id, observer_id);
	sync_core(&data);
	wait_for_state(&data, &data.producer, PW_STREAM_STATE_STREAMING);
	wait_for_state(&data, &data.observer, PW_STREAM_STATE_STREAMING);

	trigger_producer(&data, 1);
	trigger_observer(&data, 1);
	CHECK(data.observer.held != NULL && data.observer.held_sequence == 1);
	CHECK(same_identity(&data.producer.identity[1],
			&data.observer.identity[1]));

	/* Withdraw the input pool while the remote observer holds a lease.  Its
	 * imported descriptor must keep the old bytes valid until the output pool
	 * is replaced. */
	pw_proxy_destroy(data.capture_link);
	data.capture_link = NULL;
	sync_core(&data);
	for (uint32_t i = 0; i < 8; i++)
		iterate_main_loop(&data);
	CHECK(validate_observer_buffer(&data.observer, data.observer.held,
			&sequence));
	CHECK(sequence == 1);

	/* Reconnect before returning the held buffer.  Native protocol transport
	 * must revoke the old output pool and marshal the new generation's FDs;
	 * changing a local spa_data.fd would not satisfy these assertions. */
	removals = atomic_load_explicit(&data.observer.removals,
			memory_order_acquire);
	data.capture_link = create_link(&data, producer_id, data.input.id);
	sync_core(&data);
	wait_for_state(&data, &data.producer, PW_STREAM_STATE_STREAMING);
	wait_for_counter(&data, &data.observer.removals, removals + 1u);
	CHECK(data.observer.held == NULL);
	CHECK(data.input.id != 0 && data.output.id != 0);

	trigger_producer(&data, 2);
	trigger_observer(&data, 2);
	CHECK(data.observer.held != NULL && data.observer.held_sequence == 2);
	CHECK(same_identity(&data.producer.identity[2],
			&data.observer.identity[2]));
	CHECK(!same_identity(&data.producer.identity[1],
			&data.producer.identity[2]));
	release_observer(&data);
	CHECK(atomic_load_explicit(&data.producer.errors,
			memory_order_relaxed) == 0);
	CHECK(atomic_load_explicit(&data.observer.errors,
			memory_order_relaxed) == 0);

	pw_proxy_destroy(data.capture_link);
	pw_proxy_destroy(data.playback_link);
	pw_stream_destroy(data.observer.stream);
	pw_stream_destroy(data.producer.stream);
	spa_hook_remove(&data.input.listener);
	spa_hook_remove(&data.output.listener);
	pw_properties_free(data.input.properties);
	pw_properties_free(data.output.properties);
	pw_proxy_destroy((struct pw_proxy *)data.input.node);
	pw_proxy_destroy((struct pw_proxy *)data.output.node);
	pw_impl_module_destroy(data.module);
	pw_proxy_destroy((struct pw_proxy *)data.registry);
	pw_core_disconnect(data.core);
	pw_loop_leave(pw_main_loop_get_loop(data.main_loop));
	pw_context_destroy(data.context);
	pw_main_loop_destroy(data.main_loop);
	pw_deinit();
	return 0;
}
