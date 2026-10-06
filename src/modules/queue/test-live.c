/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/impl.h>
#include <pipewire/impl-link.h>

#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>

#define PAYLOAD_WORDS 16u
#define MAX_SEQUENCE 25050u
#define MAX_DELIVERIES 32u
#define BENCHMARK_WARMUP 1000u
#define BENCHMARK_SAMPLES 10000u
#define TIMEOUT_NSEC (5u * SPA_NSEC_PER_SEC)

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct file_identity {
	dev_t device;
	ino_t inode;
	bool valid;
};

struct endpoint {
	struct pw_stream *stream;
	struct spa_hook listener;
	_Atomic int state;
	_Atomic uint32_t errors;
	_Atomic uint32_t trigger_done;
	_Atomic uint64_t trigger_done_nsec;
	bool video_format;
};

struct producer {
	struct endpoint endpoint;
	_Atomic uint32_t requested;
	_Atomic uint32_t publications;
	_Atomic uint32_t process_cycles;
	_Atomic uint64_t process_start_nsec;
	_Atomic uint64_t process_finish_nsec;
	_Atomic bool record_identity;
	struct file_identity *identity;
};

struct observer {
	struct endpoint endpoint;
	struct producer *producer;
	_Atomic uint32_t deliveries;
	_Atomic uint32_t removals;
	_Atomic uint32_t hold;
	struct pw_buffer *held;
	uint32_t held_sequence;
	uint32_t sequence[MAX_DELIVERIES];
	bool lease_storage;
};

struct node_search {
	const char *name;
	struct pw_impl_node *node;
};

struct fixture {
	struct pw_main_loop *main_loop;
	struct pw_context *context;
	struct pw_core *core;
	struct pw_impl_module *module;
	struct pw_impl_link *capture_link;
	struct pw_impl_link *playback_link;
	struct producer producer;
	struct observer observer;
	bool video_format;
};

struct latency_sample {
	uint64_t total;
	uint64_t trigger_call;
	uint64_t dispatch;
	uint64_t source_process;
	uint64_t graph_completion;
	uint32_t cycles;
};

static uint64_t monotonic_nsec(void)
{
	struct timespec now;

	CHECK(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static uint64_t endpoint_timestamp(struct endpoint *endpoint)
{
	struct timespec now;

	if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
		atomic_fetch_add_explicit(&endpoint->errors, 1,
				memory_order_relaxed);
		return 0;
	}
	return (uint64_t)now.tv_sec * SPA_NSEC_PER_SEC + (uint64_t)now.tv_nsec;
}

static void iterate_main_loop(struct fixture *fixture)
{
	struct pw_loop *loop = pw_main_loop_get_loop(fixture->main_loop);

	(void)pw_loop_iterate(loop, 10);
}

static int find_node(void *data, struct pw_global *global)
{
	struct node_search *search = data;
	struct pw_impl_node *node;
	const struct pw_properties *properties;
	const char *name;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		return 0;
	node = pw_global_get_object(global);
	if (node == NULL)
		return 0;
	properties = pw_impl_node_get_properties(node);
	name = pw_properties_get(properties, PW_KEY_NODE_NAME);
	if (name != NULL && strcmp(name, search->name) == 0) {
		search->node = node;
		return 1;
	}
	return 0;
}

static struct pw_impl_node *wait_for_node(struct fixture *fixture,
		const char *name)
{
	struct node_search search = { .name = name };
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (monotonic_nsec() < deadline) {
		search.node = NULL;
		(void)pw_context_for_each_global(fixture->context, find_node, &search);
		if (search.node != NULL)
			return search.node;
		iterate_main_loop(fixture);
	}
	fprintf(stderr, "timed out waiting for node %s\n", name);
	abort();
}

static int count_port(void *data, struct pw_impl_port *port)
{
	uint32_t *count = data;

	(void)port;
	(*count)++;
	return 0;
}

static void check_initial_queue_ports(struct pw_impl_node *capture_node,
		struct pw_impl_node *playback_node)
{
	uint32_t capture_inputs = 0, playback_outputs = 0;

	CHECK(pw_impl_node_for_each_port(capture_node, PW_DIRECTION_INPUT,
			count_port, &capture_inputs) == 0);
	CHECK(pw_impl_node_for_each_port(playback_node, PW_DIRECTION_OUTPUT,
			count_port, &playback_outputs) == 0);
	CHECK(capture_inputs == 1);
	CHECK(playback_outputs == 1);
}

static void wait_for_streaming(struct fixture *fixture,
		struct endpoint *endpoint)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (monotonic_nsec() < deadline) {
		int state = atomic_load_explicit(&endpoint->state,
				memory_order_acquire);

		CHECK(state != PW_STREAM_STATE_ERROR);
		if (state == PW_STREAM_STATE_STREAMING)
			return;
		iterate_main_loop(fixture);
	}
	fprintf(stderr, "timed out waiting for stream state, current=%d\n",
			atomic_load_explicit(&endpoint->state, memory_order_relaxed));
	abort();
}

static void wait_for_counter(struct fixture *fixture, _Atomic uint32_t *value,
		uint32_t expected, const char *description)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (monotonic_nsec() < deadline) {
		if (atomic_load_explicit(value, memory_order_acquire) >= expected)
			return;
		iterate_main_loop(fixture);
	}
	fprintf(stderr, "timed out waiting for %s: expected=%u actual=%u\n",
			description, expected,
			atomic_load_explicit(value, memory_order_relaxed));
	abort();
}

static bool get_file_identity(const struct spa_data *data,
		struct file_identity *identity)
{
	struct stat status;

	if (data->fd < 0 || fstat(data->fd, &status) < 0)
		return false;
	identity->device = status.st_dev;
	identity->inode = status.st_ino;
	identity->valid = true;
	return true;
}

static bool same_identity(const struct file_identity *left,
		const struct file_identity *right)
{
	return left->valid && right->valid && left->device == right->device &&
			left->inode == right->inode;
}

static void endpoint_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct endpoint *endpoint = data;

	(void)old;
	if (state == PW_STREAM_STATE_ERROR) {
		fprintf(stderr, "stream error: %s\n", error == NULL ? "unknown" : error);
		atomic_fetch_add_explicit(&endpoint->errors, 1, memory_order_relaxed);
	}
	atomic_store_explicit(&endpoint->state, state, memory_order_release);
}

static void endpoint_trigger_done(void *data)
{
	struct endpoint *endpoint = data;

	atomic_store_explicit(&endpoint->trigger_done_nsec,
			endpoint_timestamp(endpoint), memory_order_relaxed);
	atomic_fetch_add_explicit(&endpoint->trigger_done, 1,
			memory_order_release);
}

static void producer_process(void *data)
{
	struct producer *producer = data;
	struct pw_buffer *pw_buffer;
	struct spa_buffer *buffer;
	struct spa_meta_header *header;
	struct file_identity identity = { 0 };
	uint32_t requested, sequence, i;
	uint32_t *payload;

	atomic_store_explicit(&producer->process_start_nsec,
			endpoint_timestamp(&producer->endpoint), memory_order_relaxed);
	requested = atomic_load_explicit(&producer->requested,
			memory_order_acquire);
	while (requested != 0 && !atomic_compare_exchange_weak_explicit(
			&producer->requested, &requested, requested - 1u,
			memory_order_acq_rel, memory_order_relaxed))
		;
	if (requested == 0)
		goto done;
	pw_buffer = pw_stream_dequeue_buffer(producer->endpoint.stream);
	if (pw_buffer == NULL) {
		atomic_fetch_add_explicit(&producer->requested, 1,
				memory_order_release);
		goto done;
	}
	buffer = pw_buffer->buffer;
	if (buffer->n_datas != 1 || buffer->datas[0].data == NULL ||
			buffer->datas[0].chunk == NULL ||
			buffer->datas[0].maxsize < PAYLOAD_WORDS * sizeof(uint32_t)) {
		atomic_fetch_add_explicit(&producer->endpoint.errors, 1,
				memory_order_relaxed);
		(void)pw_stream_queue_buffer(producer->endpoint.stream, pw_buffer);
		goto done;
	}
	sequence = atomic_fetch_add_explicit(&producer->publications, 1,
			memory_order_relaxed) + 1u;
	if (sequence >= MAX_SEQUENCE) {
		atomic_fetch_add_explicit(&producer->endpoint.errors, 1,
				memory_order_relaxed);
		(void)pw_stream_queue_buffer(producer->endpoint.stream, pw_buffer);
		goto done;
	}
	payload = buffer->datas[0].data;
	for (i = 0; i < PAYLOAD_WORDS; i++)
		payload[i] = sequence * 1000u + i;
	buffer->datas[0].chunk->offset = 0;
	buffer->datas[0].chunk->size = PAYLOAD_WORDS * sizeof(uint32_t);
	buffer->datas[0].chunk->stride = producer->endpoint.video_format ?
			PAYLOAD_WORDS * sizeof(uint32_t) : sizeof(uint32_t);
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL) {
		atomic_fetch_add_explicit(&producer->endpoint.errors, 1,
				memory_order_relaxed);
	} else {
		header->flags = 0;
		header->offset = 0;
		header->pts = (int64_t)sequence;
		header->dts_offset = 0;
		header->seq = sequence;
	}
	if (atomic_load_explicit(&producer->record_identity,
			memory_order_acquire)) {
		(void)get_file_identity(&buffer->datas[0], &identity);
		producer->identity[sequence] = identity;
	}
	if (pw_stream_queue_buffer(producer->endpoint.stream, pw_buffer) < 0)
		atomic_fetch_add_explicit(&producer->endpoint.errors, 1,
				memory_order_relaxed);
done:
	atomic_store_explicit(&producer->process_finish_nsec,
			endpoint_timestamp(&producer->endpoint), memory_order_relaxed);
	atomic_fetch_add_explicit(&producer->process_cycles, 1,
			memory_order_release);
}

static bool validate_observer_buffer(struct pw_buffer *pw_buffer,
		uint32_t *sequence, bool video_format)
{
	struct spa_buffer *buffer = pw_buffer->buffer;
	struct spa_meta_header *header;
	uint32_t *payload, i;

	if (buffer->n_datas != 1 || buffer->datas[0].data == NULL ||
			buffer->datas[0].chunk == NULL ||
			buffer->datas[0].chunk->offset != 0 ||
			buffer->datas[0].chunk->size !=
				PAYLOAD_WORDS * sizeof(uint32_t) ||
			buffer->datas[0].chunk->stride != (int32_t)(video_format ?
					PAYLOAD_WORDS * sizeof(uint32_t) : sizeof(uint32_t)))
		return false;
	header = spa_buffer_find_meta_data(buffer, SPA_META_Header,
			sizeof(*header));
	if (header == NULL || header->seq == 0 || header->seq >= MAX_SEQUENCE ||
			header->pts != (int64_t)header->seq)
		return false;
	*sequence = header->seq;
	payload = SPA_PTROFF(buffer->datas[0].data,
			buffer->datas[0].chunk->offset, uint32_t);
	for (i = 0; i < PAYLOAD_WORDS; i++)
		if (payload[i] != *sequence * 1000u + i)
			return false;
	return true;
}

static void observer_process(void *data)
{
	struct observer *observer = data;
	struct pw_buffer *pw_buffer;

	while ((pw_buffer = pw_stream_dequeue_buffer(
			observer->endpoint.stream)) != NULL) {
		struct file_identity identity = { 0 };
		struct file_identity *source_identity;
		uint32_t delivery, sequence = 0;
		bool same;

		if (!validate_observer_buffer(pw_buffer, &sequence,
				observer->endpoint.video_format)) {
			atomic_fetch_add_explicit(&observer->endpoint.errors, 1,
					memory_order_relaxed);
			(void)pw_stream_queue_buffer(observer->endpoint.stream, pw_buffer);
			continue;
		}
		delivery = atomic_load_explicit(&observer->deliveries,
				memory_order_relaxed);
		if (delivery >= MAX_DELIVERIES) {
			atomic_fetch_add_explicit(&observer->endpoint.errors, 1,
					memory_order_relaxed);
			(void)pw_stream_queue_buffer(observer->endpoint.stream, pw_buffer);
			continue;
		}
		observer->sequence[delivery] = sequence;
		(void)get_file_identity(&pw_buffer->buffer->datas[0], &identity);
		same = false;
		if (sequence < MAX_SEQUENCE && observer->producer != NULL) {
			source_identity = &observer->producer->identity[sequence];
			same = same_identity(source_identity, &identity);
		}
		if (same != observer->lease_storage)
			atomic_fetch_add_explicit(&observer->endpoint.errors, 1,
					memory_order_relaxed);
		if (atomic_load_explicit(&observer->hold, memory_order_acquire) != 0 &&
				observer->held == NULL) {
			observer->held = pw_buffer;
			observer->held_sequence = sequence;
			atomic_store_explicit(&observer->deliveries, delivery + 1u,
					memory_order_release);
			continue;
		}
		if (pw_stream_queue_buffer(observer->endpoint.stream, pw_buffer) < 0)
			atomic_fetch_add_explicit(&observer->endpoint.errors, 1,
					memory_order_relaxed);
		atomic_store_explicit(&observer->deliveries, delivery + 1u,
				memory_order_release);
	}
}

static void observer_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct observer *observer = data;

	/* A generation change is allowed to revoke the downstream pool.  Once
	 * remove_buffer identifies an application-held buffer, the application
	 * must drop that pointer instead of returning it through the new pool. */
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

static struct spa_pod *build_format(uint8_t *storage, size_t size,
		uint32_t object_id, bool video_format)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { (int32_t)PAYLOAD_WORDS };
	const struct spa_fraction rate = SPA_FRACTION(1000, 1);
	const struct spa_rectangle video_size =
			SPA_RECTANGLE(PAYLOAD_WORDS * sizeof(uint32_t), 1);

	if (video_format)
		return spa_pod_builder_add_object(&builder,
				SPA_TYPE_OBJECT_Format, object_id,
				SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
				SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
				SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_GRAY8),
				SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&video_size),
				SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate));

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, object_id,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String("org.pipewireao.test.queue/1"),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_U32_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_ROW_MAJOR),
			SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction(&rate));
}

static struct pw_stream *create_endpoint_stream(struct fixture *fixture,
		const char *name, enum pw_direction direction,
		const struct pw_stream_events *events, struct endpoint *endpoint)
{
	uint8_t storage[1024];
	uint8_t buffers_storage[256];
	uint8_t header_storage[256];
	struct spa_pod_builder buffers_builder = SPA_POD_BUILDER_INIT(
			buffers_storage, sizeof(buffers_storage));
	struct spa_pod_builder header_builder = SPA_POD_BUILDER_INIT(
			header_storage, sizeof(header_storage));
	struct spa_pod *format = build_format(storage, sizeof(storage),
			SPA_PARAM_EnumFormat, fixture->video_format);
	const struct spa_pod *params[3];
	struct pw_stream *stream;

	CHECK(format != NULL);
	stream = pw_stream_new(fixture->core, name,
			pw_properties_new(PW_KEY_NODE_NAME, name,
					PW_KEY_NODE_LOOP_NAME, direction == PW_DIRECTION_OUTPUT ?
							"queue.capture" : "queue.playback",
					PW_KEY_NODE_VIRTUAL, "true",
					PW_KEY_NODE_PAUSE_ON_IDLE, "false", NULL));
	CHECK(stream != NULL);
	endpoint->stream = stream;
	endpoint->video_format = fixture->video_format;
	atomic_init(&endpoint->state, PW_STREAM_STATE_UNCONNECTED);
	atomic_init(&endpoint->errors, 0);
	atomic_init(&endpoint->trigger_done, 0);
	atomic_init(&endpoint->trigger_done_nsec, 0);
	pw_stream_add_listener(stream, &endpoint->listener, events, endpoint);
	params[0] = format;
	params[1] = spa_pod_builder_add_object(&buffers_builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_Int(3),
			SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,
			SPA_POD_Int(PAYLOAD_WORDS * (int32_t)sizeof(uint32_t)),
			SPA_PARAM_BUFFERS_stride,
			SPA_POD_Int(fixture->video_format ?
					PAYLOAD_WORDS * sizeof(uint32_t) : sizeof(uint32_t)),
			SPA_PARAM_BUFFERS_align, SPA_POD_Int(16),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(1u << SPA_DATA_MemFd));
	params[2] = spa_pod_builder_add_object(&header_builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int((int32_t)sizeof(struct spa_meta_header)));
	CHECK(params[1] != NULL && params[2] != NULL);
	CHECK(pw_stream_connect(stream, direction, PW_ID_ANY,
			PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS | PW_STREAM_FLAG_NO_CONVERT |
			PW_STREAM_FLAG_RT_TRIGGER_DONE,
			params, SPA_N_ELEMENTS(params)) == 0);
	return stream;
}

static struct pw_impl_link *link_nodes(struct fixture *fixture,
		struct pw_impl_node *output_node, struct pw_impl_node *input_node)
{
	struct pw_impl_port *output = pw_impl_node_find_port(output_node,
			PW_DIRECTION_OUTPUT, 0);
	struct pw_impl_port *input = pw_impl_node_find_port(input_node,
			PW_DIRECTION_INPUT, 0);
	struct pw_impl_link *link;

	CHECK(output != NULL);
	CHECK(input != NULL);
	link = pw_context_create_link(fixture->context, output, input,
			NULL, NULL, 0);
	CHECK(link != NULL);
	CHECK(pw_impl_link_register(link, NULL) == 0);
	CHECK(pw_impl_node_set_active(output_node, true) == 0);
	CHECK(pw_impl_node_set_active(input_node, true) == 0);
	return link;
}

static void wait_for_link(struct fixture *fixture, struct pw_impl_link *link)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (monotonic_nsec() < deadline) {
		const struct pw_link_info *info = pw_impl_link_get_info(link);

		CHECK(info != NULL);
		CHECK(info->state != PW_LINK_STATE_ERROR);
		if (info->state == PW_LINK_STATE_ACTIVE ||
				info->state == PW_LINK_STATE_PAUSED)
			return;
		iterate_main_loop(fixture);
	}
	fprintf(stderr, "timed out waiting for link\n");
	abort();
}

static uint64_t module_counter(struct fixture *fixture, const char *name)
{
	const struct pw_properties *properties =
			pw_impl_module_get_properties(fixture->module);
	const char *value = pw_properties_get(properties, name);
	char *end = NULL;
	uint64_t result;

	CHECK(value != NULL);
	errno = 0;
	result = strtoull(value, &end, 10);
	CHECK(errno == 0 && end != value && *end == '\0');
	return result;
}

static void wait_for_stats(struct fixture *fixture, uint64_t publications)
{
	uint64_t deadline = monotonic_nsec() + 2u * SPA_NSEC_PER_SEC;

	while (monotonic_nsec() < deadline) {
		iterate_main_loop(fixture);
		if (module_counter(fixture, "queue.stats.publications") == publications)
			return;
	}
	fprintf(stderr, "timed out waiting for module statistics\n");
	abort();
}

static void trigger_producer(struct fixture *fixture, uint32_t expected)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	atomic_fetch_add_explicit(&fixture->producer.requested, 1,
			memory_order_release);
	while (atomic_load_explicit(&fixture->producer.publications,
			memory_order_acquire) < expected && monotonic_nsec() < deadline) {
		uint32_t trigger_done = atomic_load_explicit(
				&fixture->producer.endpoint.trigger_done,
				memory_order_acquire);
		int result = pw_stream_trigger_process(
				fixture->producer.endpoint.stream);

		if (result < 0)
			fprintf(stderr, "producer trigger failed: %s driving=%d\n",
					spa_strerror(result), pw_stream_is_driving(
						fixture->producer.endpoint.stream));
		CHECK(result >= 0);
		wait_for_counter(fixture, &fixture->producer.endpoint.trigger_done,
				trigger_done + 1u, "producer graph cycle");
	}
	if (atomic_load_explicit(&fixture->producer.publications,
			memory_order_acquire) < expected)
		fprintf(stderr, "producer stalled: wanted=%u got=%u\n", expected,
				atomic_load_explicit(&fixture->producer.publications,
					memory_order_relaxed));
	CHECK(atomic_load_explicit(&fixture->producer.publications,
			memory_order_acquire) >= expected);
	CHECK(atomic_load_explicit(&fixture->producer.endpoint.errors,
			memory_order_relaxed) == 0);
}

static void trigger_observer(struct fixture *fixture, uint32_t expected)
{
	uint64_t deadline = monotonic_nsec() + TIMEOUT_NSEC;

	while (atomic_load_explicit(&fixture->observer.deliveries,
			memory_order_acquire) < expected && monotonic_nsec() < deadline) {
		uint32_t trigger_done = atomic_load_explicit(
				&fixture->observer.endpoint.trigger_done,
				memory_order_acquire);
		int result = pw_stream_trigger_process(
				fixture->observer.endpoint.stream);

		if (result < 0)
			fprintf(stderr, "observer trigger failed: %s driving=%d\n",
					spa_strerror(result), pw_stream_is_driving(
						fixture->observer.endpoint.stream));
		CHECK(result >= 0);
		/* A graph rebuilt after format withdrawal can accept a trigger before
		 * the new driver activation has reached its data loop. Keep driving the
		 * bounded test graph until one trigger completes or the overall
		 * delivery deadline expires. */
		for (uint32_t iteration = 0; iteration < 64u &&
				atomic_load_explicit(
					&fixture->observer.endpoint.trigger_done,
					memory_order_acquire) == trigger_done;
				iteration++)
			iterate_main_loop(fixture);
	}
	if (atomic_load_explicit(&fixture->observer.deliveries,
			memory_order_acquire) < expected)
		fprintf(stderr,
				"observer stalled: wanted=%u got=%u held=%p pubs=%" PRIu64
				" deliveries=%" PRIu64 " completions=%" PRIu64
				" pool=%" PRIu64 " errors=%" PRIu64 "\n",
				expected, atomic_load_explicit(&fixture->observer.deliveries,
					memory_order_relaxed), (void *)fixture->observer.held,
				module_counter(fixture, "queue.stats.publications"),
				module_counter(fixture, "queue.stats.deliveries"),
				module_counter(fixture, "queue.stats.completions"),
				module_counter(fixture, "queue.stats.pool-exhaustions"),
				module_counter(fixture, "queue.stats.protocol-errors"));
	CHECK(atomic_load_explicit(&fixture->observer.deliveries,
			memory_order_acquire) >= expected);
	CHECK(atomic_load_explicit(&fixture->observer.endpoint.errors,
			memory_order_relaxed) == 0);
}

static void release_observer(struct fixture *fixture)
{
	uint32_t sequence;

	/* The graph cycle is complete, so its data loop no longer owns this buffer. */
	CHECK(fixture->observer.held != NULL);
	CHECK(validate_observer_buffer(fixture->observer.held, &sequence,
			fixture->observer.endpoint.video_format));
	CHECK(sequence == fixture->observer.held_sequence);
	CHECK(pw_stream_queue_buffer(fixture->observer.endpoint.stream,
			fixture->observer.held) == 0);
	fixture->observer.held = NULL;
	fixture->observer.held_sequence = 0;
}

static void fixture_init_format(struct fixture *fixture, const char *overflow,
		const char *storage, bool video_format, bool observer_first)
{
	char args[512];
	const char *capture_queue_id, *playback_queue_id;
	int length;
	struct pw_impl_node *producer_node, *capture_node, *playback_node,
			*observer_node;

	memset(fixture, 0, sizeof(*fixture));
	fixture->video_format = video_format;
	fixture->main_loop = pw_main_loop_new(NULL);
	CHECK(fixture->main_loop != NULL);
	fixture->context = pw_context_new(pw_main_loop_get_loop(fixture->main_loop),
			pw_properties_new(PW_KEY_CONFIG_NAME, "pipewire.conf",
					"context.data-loops", "[ { loop.name=queue.capture thread.name=queue.capture } "
							"{ loop.name=queue.playback thread.name=queue.playback } ]",
					"module.rt", "false",
					"module.profiler", "false",
					"factory.dummy-driver", "false",
					"factory.freewheel-driver", "false", NULL), 0);
	CHECK(fixture->context != NULL);
	fixture->core = pw_context_connect_self(fixture->context, NULL, 0);
	CHECK(fixture->core != NULL);
	length = snprintf(args, sizeof(args),
			"queue.max-buffers=1 queue.overflow=%s queue.storage=%s "
			"queue.media=%s "
			"remote.name=internal "
			"capture.props={ node.name=test.queue-input node.loop.name=queue.capture } "
			"playback.props={ node.name=test.queue-output node.loop.name=queue.playback }",
			overflow, storage,
			video_format ? "video/raw" : "application/ndarray");
	CHECK(length > 0 && (size_t)length < sizeof(args));
	fixture->module = pw_context_load_module(fixture->context,
			"libpipewire-module-queue", args, NULL);
	CHECK(fixture->module != NULL);
	/* Keep the loop entered exactly as pw_main_loop_run() does.  Releasing it
	 * between manual iterations permits cross-loop invokes to acquire the main
	 * loop mutex from a data-loop callback. */
	pw_loop_enter(pw_main_loop_get_loop(fixture->main_loop));
	/* Both deployment identities exist before either external endpoint is
	 * linked.  Their pools and formats are negotiated later. */
	capture_node = wait_for_node(fixture, "test.queue-input");
	playback_node = wait_for_node(fixture, "test.queue-output");
	check_initial_queue_ports(capture_node, playback_node);
	capture_queue_id = pw_properties_get(
			pw_impl_node_get_properties(capture_node),
			PWAO_QUEUE_ID_PROPERTY);
	playback_queue_id = pw_properties_get(
			pw_impl_node_get_properties(playback_node),
			PWAO_QUEUE_ID_PROPERTY);
	CHECK(capture_queue_id != NULL);
	CHECK(playback_queue_id != NULL);
	CHECK(strcmp(capture_queue_id, playback_queue_id) == 0);
	fixture->producer.identity = calloc(MAX_SEQUENCE,
			sizeof(*fixture->producer.identity));
	CHECK(fixture->producer.identity != NULL);
	atomic_init(&fixture->producer.requested, 0);
	atomic_init(&fixture->producer.publications, 0);
	atomic_init(&fixture->producer.process_cycles, 0);
	atomic_init(&fixture->producer.process_start_nsec, 0);
	atomic_init(&fixture->producer.process_finish_nsec, 0);
	atomic_init(&fixture->producer.record_identity, true);
	atomic_init(&fixture->observer.deliveries, 0);
	atomic_init(&fixture->observer.removals, 0);
	atomic_init(&fixture->observer.hold, 1);
	fixture->observer.producer = &fixture->producer;
	fixture->observer.lease_storage = strcmp(storage, "lease") == 0;
	if (observer_first) {
		create_endpoint_stream(fixture, "test.queue-observer",
				PW_DIRECTION_INPUT, &observer_events,
				&fixture->observer.endpoint);
		observer_node = wait_for_node(fixture, "test.queue-observer");
		fixture->playback_link = link_nodes(fixture, playback_node,
				observer_node);
	}
	create_endpoint_stream(fixture, "test.queue-producer", PW_DIRECTION_OUTPUT,
			&producer_events, &fixture->producer.endpoint);
	producer_node = wait_for_node(fixture, "test.queue-producer");
	fixture->capture_link = link_nodes(fixture, producer_node, capture_node);
	wait_for_link(fixture, fixture->capture_link);
	if (!observer_first) {
		create_endpoint_stream(fixture, "test.queue-observer",
				PW_DIRECTION_INPUT, &observer_events,
				&fixture->observer.endpoint);
		observer_node = wait_for_node(fixture, "test.queue-observer");
		fixture->playback_link = link_nodes(fixture, playback_node,
				observer_node);
	}
	wait_for_link(fixture, fixture->playback_link);
	wait_for_streaming(fixture, &fixture->producer.endpoint);
	wait_for_streaming(fixture, &fixture->observer.endpoint);
	CHECK(pw_stream_get_data_loop(fixture->producer.endpoint.stream) != NULL);
	CHECK(pw_stream_get_data_loop(fixture->observer.endpoint.stream) != NULL);
	fprintf(stderr, "queue test data-loops producer=%s observer=%s\n",
			pw_stream_get_data_loop(fixture->producer.endpoint.stream)->name,
			pw_stream_get_data_loop(fixture->observer.endpoint.stream)->name);
	CHECK(pw_stream_get_data_loop(fixture->producer.endpoint.stream) !=
			pw_stream_get_data_loop(fixture->observer.endpoint.stream));
}

static void fixture_init(struct fixture *fixture, const char *overflow,
		const char *storage)
{
	fixture_init_format(fixture, overflow, storage, false, false);
}

static void fixture_clear(struct fixture *fixture)
{
	if (fixture->observer.held != NULL) {
		atomic_store_explicit(&fixture->observer.hold, 0,
				memory_order_release);
		release_observer(fixture);
	}
	if (fixture->playback_link != NULL)
		pw_impl_link_destroy(fixture->playback_link);
	if (fixture->capture_link != NULL)
		pw_impl_link_destroy(fixture->capture_link);
	if (fixture->observer.endpoint.stream != NULL)
		pw_stream_destroy(fixture->observer.endpoint.stream);
	if (fixture->producer.endpoint.stream != NULL)
		pw_stream_destroy(fixture->producer.endpoint.stream);
	if (fixture->module != NULL)
		pw_impl_module_destroy(fixture->module);
	if (fixture->core != NULL)
		pw_core_disconnect(fixture->core);
	pw_loop_leave(pw_main_loop_get_loop(fixture->main_loop));
	pw_context_destroy(fixture->context);
	pw_main_loop_destroy(fixture->main_loop);
	free(fixture->producer.identity);
}

static void test_drop_policy(const char *overflow, const char *storage,
		uint32_t expected_second, uint64_t replacements,
		uint64_t dropped_arrivals)
{
	struct fixture fixture;
	uint32_t sequence;

	fixture_init(&fixture, overflow, storage);
	trigger_producer(&fixture, 1);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.sequence[0] == 1);
	for (sequence = 2; sequence <= 9; sequence++)
		trigger_producer(&fixture, sequence);
	release_observer(&fixture);
	trigger_observer(&fixture, 2);
	CHECK(fixture.observer.sequence[1] == expected_second);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	wait_for_stats(&fixture, 9);
	CHECK(module_counter(&fixture, "queue.stats.replacements") == replacements);
	CHECK(module_counter(&fixture, "queue.stats.dropped-arrivals") ==
			dropped_arrivals);
	CHECK(module_counter(&fixture, "queue.stats.backpressure") == 0);
	CHECK(module_counter(&fixture, "queue.stats.deliveries") == 2);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_video_format(void)
{
	struct fixture fixture;

	fixture_init_format(&fixture, "drop-oldest", "copy", true, false);
	trigger_producer(&fixture, 1);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.sequence[0] == 1);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_observer_first(const char *storage)
{
	struct fixture fixture;

	fixture_init_format(&fixture, "drop-oldest", storage, false, true);
	trigger_producer(&fixture, 1);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.sequence[0] == 1);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_backpressure(const char *storage)
{
	struct fixture fixture;

	fixture_init(&fixture, "backpressure", storage);
	trigger_producer(&fixture, 1);
	trigger_observer(&fixture, 1);
	trigger_producer(&fixture, 2);
	trigger_producer(&fixture, 3);
	release_observer(&fixture);
	trigger_observer(&fixture, 2);
	CHECK(fixture.observer.sequence[1] == 2);
	release_observer(&fixture);
	trigger_observer(&fixture, 3);
	CHECK(fixture.observer.sequence[2] == 3);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	wait_for_stats(&fixture, 3);
	CHECK(module_counter(&fixture, "queue.stats.backpressure") == 1);
	CHECK(module_counter(&fixture, "queue.stats.replacements") == 0);
	CHECK(module_counter(&fixture, "queue.stats.dropped-arrivals") == 0);
	CHECK(module_counter(&fixture, "queue.stats.deliveries") == 3);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_module_destruction(const char *storage,
		const char *occupancy, bool context_first)
{
	struct fixture fixture;
	bool held = false;

	fixture_init(&fixture,
			strcmp(occupancy, "backpressure") == 0 ?
					"backpressure" : "drop-oldest",
			storage);
	if (strcmp(occupancy, "queued") == 0) {
		trigger_producer(&fixture, 1);
	} else if (strcmp(occupancy, "in-flight") == 0 ||
			strcmp(occupancy, "backpressure") == 0) {
		trigger_producer(&fixture, 1);
		trigger_observer(&fixture, 1);
		held = true;
		if (strcmp(occupancy, "backpressure") == 0) {
			trigger_producer(&fixture, 2);
			trigger_producer(&fixture, 3);
		}
	} else {
		CHECK(strcmp(occupancy, "empty") == 0);
	}
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	if (context_first) {
		/* Core-owned stream destruction occurs before module destruction.
		 * In-flight observer loans must be revoked, never returned afterward. */
		pw_loop_leave(pw_main_loop_get_loop(fixture.main_loop));
		pw_context_destroy(fixture.context);
		CHECK(fixture.observer.held == NULL);
		CHECK(atomic_load_explicit(&fixture.producer.endpoint.errors,
				memory_order_relaxed) == 0);
		CHECK(atomic_load_explicit(&fixture.observer.endpoint.errors,
				memory_order_relaxed) == 0);
		pw_main_loop_destroy(fixture.main_loop);
		free(fixture.producer.identity);
		return;
	}
	pw_impl_module_destroy(fixture.module);
	fixture.module = NULL;
	/* Destroying the module also destroys both internal stream ports and their
	 * links. An application-held observer buffer is revoked with that link and
	 * must not be returned through the now-disconnected stream. */
	fixture.capture_link = NULL;
	fixture.playback_link = NULL;
	if (held) {
		CHECK(fixture.observer.held != NULL);
		fixture.observer.held = NULL;
		fixture.observer.held_sequence = 0;
	}
	for (uint32_t i = 0; i < 4; i++)
		iterate_main_loop(&fixture);
	CHECK(atomic_load_explicit(&fixture.producer.endpoint.errors,
			memory_order_relaxed) == 0);
	CHECK(atomic_load_explicit(&fixture.observer.endpoint.errors,
			memory_order_relaxed) == 0);
	fixture_clear(&fixture);
}

static void test_format_recreation(const char *storage)
{
	struct fixture fixture;
	struct pw_impl_node *producer_node, *capture_node, *playback_node;
	struct pw_impl_link *playback_link;

	fixture_init(&fixture, "drop-oldest", storage);
	playback_node = wait_for_node(&fixture, "test.queue-output");
	playback_link = fixture.playback_link;
	trigger_producer(&fixture, 1);
	CHECK(atomic_load_explicit(&fixture.observer.deliveries,
			memory_order_relaxed) == 0);
	/* The producer link owns one input pool generation.  Removing it must not
	 * replace the output deployment identity or its downstream link. */
	pw_impl_link_destroy(fixture.capture_link);
	fixture.capture_link = NULL;
	for (uint32_t i = 0; i < 8; i++)
		iterate_main_loop(&fixture);
	CHECK(wait_for_node(&fixture, "test.queue-output") == playback_node);
	CHECK(fixture.playback_link == playback_link);
	CHECK(atomic_load_explicit(&fixture.producer.endpoint.errors,
			memory_order_relaxed) == 0);
	CHECK(atomic_load_explicit(&fixture.observer.endpoint.errors,
			memory_order_relaxed) == 0);

	producer_node = wait_for_node(&fixture, "test.queue-producer");
	capture_node = wait_for_node(&fixture, "test.queue-input");
	fixture.capture_link = link_nodes(&fixture, producer_node, capture_node);
	wait_for_link(&fixture, fixture.capture_link);
	CHECK(wait_for_node(&fixture, "test.queue-output") == playback_node);
	CHECK(fixture.playback_link == playback_link);
	wait_for_link(&fixture, playback_link);
	wait_for_streaming(&fixture, &fixture.producer.endpoint);
	wait_for_streaming(&fixture, &fixture.observer.endpoint);
	trigger_producer(&fixture, 2);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.sequence[0] == 2);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_retained_lease_withdrawal(void)
{
	struct fixture fixture;
	struct pw_impl_node *producer_node, *capture_node, *playback_node;
	struct pw_impl_link *playback_link;
	uint32_t sequence;

	fixture_init(&fixture, "drop-oldest", "lease");
	playback_node = wait_for_node(&fixture, "test.queue-output");
	playback_link = fixture.playback_link;
	trigger_producer(&fixture, 1);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.held != NULL);

	pw_impl_link_destroy(fixture.capture_link);
	fixture.capture_link = NULL;
	for (uint32_t i = 0; i < 8; i++)
		iterate_main_loop(&fixture);
	CHECK(wait_for_node(&fixture, "test.queue-output") == playback_node);
	CHECK(fixture.playback_link == playback_link);
	CHECK(validate_observer_buffer(fixture.observer.held, &sequence,
			fixture.observer.endpoint.video_format));
	CHECK(sequence == 1);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);

	producer_node = wait_for_node(&fixture, "test.queue-producer");
	capture_node = wait_for_node(&fixture, "test.queue-input");
	fixture.capture_link = link_nodes(&fixture, producer_node, capture_node);
	wait_for_link(&fixture, fixture.capture_link);
	wait_for_link(&fixture, playback_link);
	wait_for_streaming(&fixture, &fixture.producer.endpoint);
	wait_for_streaming(&fixture, &fixture.observer.endpoint);
	trigger_producer(&fixture, 2);
	trigger_observer(&fixture, 2);
	CHECK(fixture.observer.sequence[1] == 2);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_lease_generation_replacement(void)
{
	struct fixture fixture;
	struct pw_impl_node *producer_node, *capture_node, *playback_node;
	struct pw_impl_link *playback_link;
	uint32_t removals, sequence;

	fixture_init(&fixture, "drop-oldest", "lease");
	playback_node = wait_for_node(&fixture, "test.queue-output");
	playback_link = fixture.playback_link;
	trigger_producer(&fixture, 1);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.held != NULL);

	/* Input withdrawal must leave the independently owned old-generation
	 * payload valid until it is returned or the output pool is revoked. */
	pw_impl_link_destroy(fixture.capture_link);
	fixture.capture_link = NULL;
	for (uint32_t i = 0; i < 8; i++)
		iterate_main_loop(&fixture);
	CHECK(validate_observer_buffer(fixture.observer.held, &sequence,
			fixture.observer.endpoint.video_format));
	CHECK(sequence == 1);
	CHECK(wait_for_node(&fixture, "test.queue-output") == playback_node);
	CHECK(fixture.playback_link == playback_link);

	/* Reconnect while the old output lease is still held.  The same endpoint
	 * and link must replace their pool, revoke the held pointer through the
	 * observer's remove_buffer callback, and install only new-generation FDs. */
	removals = atomic_load_explicit(&fixture.observer.removals,
			memory_order_acquire);
	producer_node = wait_for_node(&fixture, "test.queue-producer");
	capture_node = wait_for_node(&fixture, "test.queue-input");
	fixture.capture_link = link_nodes(&fixture, producer_node, capture_node);
	wait_for_link(&fixture, fixture.capture_link);
	wait_for_link(&fixture, playback_link);
	wait_for_counter(&fixture, &fixture.observer.removals, removals + 1u,
			"old observer pool revocation");
	CHECK(fixture.observer.held == NULL);
	CHECK(wait_for_node(&fixture, "test.queue-output") == playback_node);
	CHECK(fixture.playback_link == playback_link);

	trigger_producer(&fixture, 2);
	trigger_observer(&fixture, 2);
	CHECK(fixture.observer.sequence[1] == 2);
	CHECK(fixture.observer.held != NULL);
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	wait_for_stats(&fixture, 2);
	CHECK(module_counter(&fixture, "queue.state.capture-generation") == 2);
	CHECK(module_counter(&fixture, "queue.state.requested-generation") == 2);
	CHECK(module_counter(&fixture, "queue.state.installed-generation") == 2);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static void test_observer_reconnect(const char *storage)
{
	struct fixture fixture;
	struct pw_impl_node *playback_node, *observer_node;
	char observer_name[64];
	uint32_t sequence = 1;

	fixture_init(&fixture, "drop-oldest", storage);
	trigger_producer(&fixture, sequence);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.held != NULL);
	for (uint32_t cycle = 0; cycle < 8; cycle++) {
		/* Destroy the slow subscriber without returning its retained buffer.
		 * This models an undocked viewport being destroyed and recreated. Link
		 * teardown must complete the output lease without allowing capture-side
		 * ownership reset to overlap an in-flight process callback. */
		fixture.playback_link = NULL;
		pw_stream_destroy(fixture.observer.endpoint.stream);
		fixture.observer.endpoint.stream = NULL;
		fixture.observer.held = NULL;
		fixture.observer.held_sequence = 0;
		atomic_store_explicit(&fixture.observer.deliveries, 0,
				memory_order_relaxed);
		atomic_store_explicit(&fixture.observer.hold, 1,
				memory_order_relaxed);
		for (uint32_t i = 0; i < 4; i++)
			iterate_main_loop(&fixture);
		playback_node = wait_for_node(&fixture, "test.queue-output");
		CHECK(snprintf(observer_name, sizeof(observer_name),
				"test.queue-observer-reattached-%u", cycle) > 0);
		create_endpoint_stream(&fixture, observer_name, PW_DIRECTION_INPUT,
				&observer_events, &fixture.observer.endpoint);
		observer_node = wait_for_node(&fixture, observer_name);
		fixture.playback_link = link_nodes(&fixture, playback_node,
				observer_node);
		wait_for_link(&fixture, fixture.playback_link);
		wait_for_streaming(&fixture, &fixture.observer.endpoint);
		trigger_producer(&fixture, ++sequence);
		trigger_observer(&fixture, 1);
		CHECK(fixture.observer.sequence[0] == sequence);
	}
	atomic_store_explicit(&fixture.observer.hold, 0, memory_order_release);
	release_observer(&fixture);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	fixture_clear(&fixture);
}

static int compare_u64(const void *left, const void *right)
{
	const uint64_t a = *(const uint64_t *)left;
	const uint64_t b = *(const uint64_t *)right;

	return (a > b) - (a < b);
}

static uint64_t percentile(const uint64_t *sorted, uint32_t count,
		uint32_t numerator, uint32_t denominator)
{
	uint64_t rank;

	CHECK(count > 0 && numerator > 0 && numerator <= denominator);
	rank = ((uint64_t)count * numerator + denominator - 1u) / denominator;
	return sorted[rank - 1u];
}

static uint64_t clock_overhead(void)
{
	uint64_t minimum = UINT64_MAX;
	uint32_t i;

	for (i = 0; i < 10000; i++) {
		uint64_t start = monotonic_nsec();
		uint64_t end = monotonic_nsec();

		minimum = SPA_MIN(minimum, end - start);
	}
	return minimum;
}

static struct latency_sample benchmark_producer_cycle(struct fixture *fixture,
		uint32_t expected)
{
	struct latency_sample sample = { 0 };
	uint64_t deadline, first_start = 0, final_complete = 0;

	atomic_fetch_add_explicit(&fixture->producer.requested, 1,
			memory_order_release);
	deadline = monotonic_nsec() + TIMEOUT_NSEC;
	while (atomic_load_explicit(&fixture->producer.publications,
			memory_order_acquire) < expected) {
		uint64_t call_start, call_finish, process_start, process_finish,
				trigger_done_nsec;
		uint32_t process_cycles = atomic_load_explicit(
				&fixture->producer.process_cycles, memory_order_acquire);
		uint32_t trigger_done = atomic_load_explicit(
				&fixture->producer.endpoint.trigger_done,
				memory_order_acquire);
		uint32_t spins = 0;
		int result;

		call_start = monotonic_nsec();
		if (first_start == 0)
			first_start = call_start;
		result = pw_stream_trigger_process(fixture->producer.endpoint.stream);
		call_finish = monotonic_nsec();
		CHECK(result >= 0);
		while (atomic_load_explicit(
				&fixture->producer.endpoint.trigger_done,
				memory_order_acquire) == trigger_done) {
			atomic_signal_fence(memory_order_seq_cst);
			if (SPA_UNLIKELY((++spins & 4095u) == 0 &&
					monotonic_nsec() >= deadline)) {
				fprintf(stderr, "benchmark producer cycle timed out: "
						"process=%u trigger-done=%u\n",
						atomic_load_explicit(
							&fixture->producer.process_cycles,
							memory_order_relaxed),
						atomic_load_explicit(
							&fixture->producer.endpoint.trigger_done,
							memory_order_relaxed));
				abort();
			}
		}
		CHECK(atomic_load_explicit(&fixture->producer.process_cycles,
				memory_order_acquire) == process_cycles + 1u);
		CHECK(atomic_load_explicit(&fixture->producer.endpoint.trigger_done,
				memory_order_acquire) == trigger_done + 1u);
		process_start = atomic_load_explicit(
				&fixture->producer.process_start_nsec, memory_order_relaxed);
		process_finish = atomic_load_explicit(
				&fixture->producer.process_finish_nsec, memory_order_relaxed);
		trigger_done_nsec = atomic_load_explicit(
				&fixture->producer.endpoint.trigger_done_nsec,
				memory_order_relaxed);
		CHECK(call_start <= call_finish && call_start <= process_start &&
				process_start <= process_finish &&
				process_finish <= trigger_done_nsec);
		sample.trigger_call += call_finish - call_start;
		sample.dispatch += process_start - call_start;
		sample.source_process += process_finish - process_start;
		sample.graph_completion += trigger_done_nsec - process_finish;
		sample.cycles++;
		final_complete = trigger_done_nsec;
	}
	CHECK(atomic_load_explicit(&fixture->producer.publications,
			memory_order_acquire) == expected);
	CHECK(atomic_load_explicit(&fixture->producer.endpoint.errors,
			memory_order_relaxed) == 0);
	CHECK(first_start > 0 && final_complete >= first_start && sample.cycles > 0);
	sample.total = final_complete - first_start;
	return sample;
}

static void print_distribution(const char *storage, const char *metric,
		uint64_t *values)
{
	qsort(values, BENCHMARK_SAMPLES, sizeof(*values), compare_u64);
	printf("storage=%s model=closed-loop metric=%s samples=%u "
			"p50=%" PRIu64 "ns p90=%" PRIu64 "ns p99=%" PRIu64
			"ns p99.9=%" PRIu64 "ns max=%" PRIu64 "ns\n",
			storage, metric, BENCHMARK_SAMPLES,
			percentile(values, BENCHMARK_SAMPLES, 50, 100),
			percentile(values, BENCHMARK_SAMPLES, 90, 100),
			percentile(values, BENCHMARK_SAMPLES, 99, 100),
			percentile(values, BENCHMARK_SAMPLES, 999, 1000),
			values[BENCHMARK_SAMPLES - 1u]);
}

static void run_producer_benchmark(const char *storage)
{
	struct fixture fixture;
	uint64_t *latency, *total, *trigger_call, *dispatch, *source_process,
			*graph_completion;
	uint64_t measurement_start = 0, measurement_end = 0;
	uint64_t total_cycles = 0;
	uint32_t i, expected = 1, max_cycles = 0;

	latency = calloc(5u * BENCHMARK_SAMPLES, sizeof(*latency));
	CHECK(latency != NULL);
	total = latency;
	trigger_call = total + BENCHMARK_SAMPLES;
	dispatch = trigger_call + BENCHMARK_SAMPLES;
	source_process = dispatch + BENCHMARK_SAMPLES;
	graph_completion = source_process + BENCHMARK_SAMPLES;
	fixture_init(&fixture, "drop-oldest", storage);
	trigger_producer(&fixture, expected);
	trigger_observer(&fixture, 1);
	CHECK(fixture.observer.sequence[0] == 1);
	atomic_store_explicit(&fixture.producer.record_identity, false,
			memory_order_release);
	for (i = 0; i < BENCHMARK_WARMUP; i++)
		(void)benchmark_producer_cycle(&fixture, ++expected);
	measurement_start = monotonic_nsec();
	for (i = 0; i < BENCHMARK_SAMPLES; i++) {
		struct latency_sample sample =
				benchmark_producer_cycle(&fixture, ++expected);

		total[i] = sample.total;
		trigger_call[i] = sample.trigger_call;
		dispatch[i] = sample.dispatch;
		source_process[i] = sample.source_process;
		graph_completion[i] = sample.graph_completion;
		total_cycles += sample.cycles;
		max_cycles = SPA_MAX(max_cycles, sample.cycles);
	}
	measurement_end = monotonic_nsec();
	release_observer(&fixture);
	wait_for_stats(&fixture, expected);
	CHECK(module_counter(&fixture, "queue.stats.replacements") ==
			BENCHMARK_WARMUP + BENCHMARK_SAMPLES - 1u);
	CHECK(module_counter(&fixture, "queue.stats.protocol-errors") == 0);
	printf("storage=%s model=closed-loop samples=%u warmup=%u rate=%.0f/s "
			"cycles=%" PRIu64 " extra-cycles=%" PRIu64
			" max-cycles=%u clock-min=%" PRIu64 "ns\n",
			storage, BENCHMARK_SAMPLES, BENCHMARK_WARMUP,
			(double)BENCHMARK_SAMPLES * (double)SPA_NSEC_PER_SEC /
				(double)(measurement_end - measurement_start),
			total_cycles, total_cycles - BENCHMARK_SAMPLES, max_cycles,
			clock_overhead());
	print_distribution(storage, "request-total", total);
	print_distribution(storage, "trigger-call-sum", trigger_call);
	print_distribution(storage, "dispatch-sum", dispatch);
	print_distribution(storage, "source-process-sum", source_process);
	print_distribution(storage, "graph-completion-sum", graph_completion);
	fixture_clear(&fixture);
	free(latency);
}

int main(int argc, char **argv)
{
	char core_name[64];
	char module_dir[PATH_MAX];
	char module_search_path[PATH_MAX * 2u];
	const char *base_module_dir;
	const char *separator;
	bool benchmark;
	int path_length;

	CHECK(argc == 2 || (argc == 3 && strcmp(argv[2], "--benchmark") == 0));
	benchmark = argc == 3;
	path_length = snprintf(core_name, sizeof(core_name),
			"pipewireao-queue-test-%ld", (long)getpid());
	CHECK(path_length > 0 && (size_t)path_length < sizeof(core_name));
	CHECK(setenv("PIPEWIREAO_CORE", core_name, 1) == 0);
	separator = strrchr(argv[1], '/');
	CHECK(separator != NULL);
	CHECK((size_t)(separator - argv[1]) < sizeof(module_dir));
	memcpy(module_dir, argv[1], (size_t)(separator - argv[1]));
	module_dir[separator - argv[1]] = '\0';
	base_module_dir = getenv("PIPEWIREAO_MODULE_DIR");
	CHECK(base_module_dir != NULL);
	path_length = snprintf(module_search_path, sizeof(module_search_path),
			"%s:%s", module_dir, base_module_dir);
	CHECK(path_length > 0 && (size_t)path_length < sizeof(module_search_path));
	CHECK(setenv("PIPEWIREAO_MODULE_DIR", module_search_path, 1) == 0);

	pw_init(&argc, &argv);
	if (benchmark) {
		run_producer_benchmark("copy");
		run_producer_benchmark("lease");
		pw_deinit();
		return 0;
	}
	test_drop_policy("drop-oldest", "copy", 9, 7, 0);
	test_drop_policy("drop-newest", "copy", 2, 0, 7);
	test_backpressure("copy");
	test_drop_policy("drop-oldest", "lease", 9, 7, 0);
	test_drop_policy("drop-newest", "lease", 2, 0, 7);
	test_backpressure("lease");
	for (uint32_t storage = 0; storage < 2; storage++) {
		const char *name = storage == 0 ? "copy" : "lease";

		fprintf(stderr, "queue lifecycle storage=%s case=observer-first\n", name);
		test_observer_first(name);
		fprintf(stderr, "queue lifecycle storage=%s case=destruction-empty\n", name);
		test_module_destruction(name, "empty", false);
		fprintf(stderr, "queue lifecycle storage=%s case=destruction-queued\n", name);
		test_module_destruction(name, "queued", false);
		fprintf(stderr, "queue lifecycle storage=%s case=destruction-in-flight\n", name);
		test_module_destruction(name, "in-flight", false);
		fprintf(stderr, "queue lifecycle storage=%s case=destruction-backpressure\n", name);
		test_module_destruction(name, "backpressure", false);
		for (uint32_t case_id = 0; case_id < 4; case_id++) {
			const char *occupancy[] = { "empty", "queued", "in-flight",
					"backpressure" };

			fprintf(stderr, "queue lifecycle storage=%s case=context-%s\n",
					name, occupancy[case_id]);
			test_module_destruction(name, occupancy[case_id], true);
		}
		fprintf(stderr, "queue lifecycle storage=%s case=observer-reconnect\n", name);
		test_observer_reconnect(name);
		fprintf(stderr, "queue lifecycle storage=%s case=format-recreation\n", name);
		test_format_recreation(name);
	}
	fprintf(stderr, "queue lifecycle storage=lease case=retained-withdrawal\n");
	test_retained_lease_withdrawal();
	fprintf(stderr, "queue lifecycle storage=lease case=generation-replacement\n");
	test_lease_generation_replacement();
	test_video_format();
	pw_deinit();
	return 0;
}
