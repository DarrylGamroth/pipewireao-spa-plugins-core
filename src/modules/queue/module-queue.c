/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "buffer-transfer.h"
#include "queue.h"

#include <errno.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>
#include <pipewire/log.h>

#define MAX_QUEUE_BUFFERS 62u
#define MAX_POOL_BUFFERS (MAX_QUEUE_BUFFERS + 2u)
#define MAX_DATA_BLOCKS PWAO_QUEUE_MAX_DATA_BLOCKS
#define MAX_METAS PWAO_QUEUE_MAX_METAS
#define PARAM_BUFFER_SIZE 16384u
#define MAX_STATS_ITEMS 40u
#define STATS_INTERVAL_NSEC SPA_NSEC_PER_SEC
#define SLOT_TOKEN_BITS 6u
#define SLOT_TOKEN_MASK ((UINT64_C(1) << SLOT_TOKEN_BITS) - 1u)
#define SLOT_TOKEN_NONE UINT64_MAX

_Static_assert(MAX_POOL_BUFFERS == (1u << SLOT_TOKEN_BITS),
		"slot token must encode every pool slot");

enum storage_mode {
	STORAGE_COPY,
	STORAGE_LEASE,
};

enum slot_state {
	SLOT_FREE,
	SLOT_PENDING,
	SLOT_ACTIVE,
	SLOT_COMPLETING,
	SLOT_BLOCKED,
};

enum failure_state {
	FAILURE_NONE,
	FAILURE_RECORDING,
	FAILURE_READY,
};

enum playback_generation_state {
	PLAYBACK_GENERATION_NONE,
	PLAYBACK_GENERATION_WAITING,
	PLAYBACK_GENERATION_READY,
};

enum ownership_action {
	OWNERSHIP_ACTION_NONE,
	OWNERSHIP_ACTION_PAUSE,
	OWNERSHIP_ACTION_WITHDRAW,
};

struct protocol_failure {
	const char *operation;
	uint32_t source_line;
	uint32_t slot;
	uint32_t expected_state;
	uint32_t observed_state;
	uint32_t pending_depth;
	uint32_t completion_depth;
	uint64_t blocked_token;
	uint64_t slot_acquisitions;
	uint64_t slot_returns;
	uint64_t expected_token;
	uint64_t observed_token;
	int result;
};

struct input_slot {
	uint32_t index;
	struct pw_buffer *capture;
	_Atomic uint32_t state;
	_Atomic uint64_t token;
	_Atomic uint64_t acquisitions;
	_Atomic uint64_t returns;
};

struct output_slot {
	uint32_t index;
	struct pw_buffer *playback;
	bool output_available;
	bool output_dequeued;
	bool output_in_flight;
	uint64_t generation;
	uint64_t delivered_input;
	int owned_fds[MAX_DATA_BLOCKS];
};

struct input_stats {
	_Atomic uint64_t publications;
	_Atomic uint64_t replacements;
	_Atomic uint64_t dropped_arrivals;
	_Atomic uint64_t backpressure;
};

struct output_stats {
	_Atomic uint64_t deliveries;
	_Atomic uint64_t completions;
	_Atomic uint64_t pool_exhaustions;
	_Atomic uint64_t protocol_errors;
};

struct impl {
	struct pw_context *context;
	struct pw_impl_module *module;
	struct pw_core *core;
	bool disconnect_core;

	struct spa_hook module_listener;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct pw_stream *playback;
	struct spa_hook playback_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;
	struct spa_pod *format;

	struct spa_source *stats_timer;
	struct pwao_queue_ring pending;
	struct pwao_queue_ring completions;
	struct input_slot inputs[MAX_POOL_BUFFERS];
	struct output_slot outputs[MAX_POOL_BUFFERS];
	uint32_t n_capture_buffers;
	uint32_t n_capture_present;
	uint32_t n_playback_buffers;
	uint32_t n_playback_present;
	_Atomic bool playback_configured;
	bool capture_pool_withdrawing;
	uint64_t capture_generation;
	uint64_t playback_requested_generation;
	_Atomic uint64_t playback_installed_generation;
	enum playback_generation_state playback_generation_state;
	_Atomic uint64_t blocked_input;
	_Atomic uint64_t next_token;
	_Atomic uint32_t active_outputs;
	uint32_t max_buffers;
	enum pwao_queue_overflow overflow;
	enum storage_mode storage;
	uint32_t media_type;
	uint32_t media_subtype;

	_Alignas(SPA_CACHE_LINE_SIZE) struct input_stats input_stats;
	_Alignas(SPA_CACHE_LINE_SIZE) struct output_stats output_stats;
	_Alignas(SPA_CACHE_LINE_SIZE) _Atomic uint32_t fatal_error;
	struct protocol_failure failure;
	bool failure_reported;
	_Atomic uint32_t ownership_requested;
	_Atomic bool ownership_in_progress;
	_Atomic uint32_t refs;
	_Atomic bool destroy_scheduled;
	_Atomic bool destroying;
	_Atomic bool cleanup_started;
};

static void impl_free(struct impl *impl);

static void impl_ref(struct impl *impl)
{
	atomic_fetch_add_explicit(&impl->refs, 1, memory_order_relaxed);
}

static void impl_unref(struct impl *impl)
{
	if (atomic_fetch_sub_explicit(&impl->refs, 1,
			memory_order_acq_rel) == 1)
		impl_free(impl);
}

static bool ownership_transition_pending(const struct impl *impl)
{
	return atomic_load_explicit(&impl->ownership_in_progress,
			memory_order_acquire) ||
		atomic_load_explicit(&impl->ownership_requested,
			memory_order_acquire) != OWNERSHIP_ACTION_NONE;
}

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "PipeWireAO contributors" },
	{ PW_KEY_MODULE_DESCRIPTION,
		"Create a bounded complete-buffer queue between graph contexts" },
	{ PW_KEY_MODULE_USAGE,
		"queue.max-buffers=<1..62> "
		"queue.overflow=<backpressure|drop-oldest|drop-newest> "
		"queue.storage=<copy|lease> "
		"queue.media=<application/ndarray|video/raw> "
		"( capture.props=<properties> ) "
		"( playback.props=<properties> )" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static void schedule_destroy(struct impl *impl)
{
	bool expected = false;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;
	if (atomic_compare_exchange_strong_explicit(&impl->destroy_scheduled,
			&expected, true, memory_order_acq_rel, memory_order_relaxed))
		pw_impl_module_schedule_destroy(impl->module);
}

static const char *slot_state_name(uint32_t state)
{
	switch (state) {
	case SLOT_FREE:
		return "free";
	case SLOT_PENDING:
		return "pending";
	case SLOT_ACTIVE:
		return "active";
	case SLOT_COMPLETING:
		return "completing";
	case SLOT_BLOCKED:
		return "blocked";
	case UINT32_MAX:
		return "n/a";
	default:
		return "invalid";
	}
}

static const char *playback_generation_state_name(
		enum playback_generation_state state)
{
	switch (state) {
	case PLAYBACK_GENERATION_NONE:
		return "none";
	case PLAYBACK_GENERATION_WAITING:
		return "waiting";
	case PLAYBACK_GENERATION_READY:
		return "ready";
	default:
		return "invalid";
	}
}

static const char *ownership_transition_name(const struct impl *impl)
{
	switch (atomic_load_explicit(&impl->ownership_requested,
			memory_order_acquire)) {
	case OWNERSHIP_ACTION_PAUSE:
		return "pause";
	case OWNERSHIP_ACTION_WITHDRAW:
		return "withdraw";
	case OWNERSHIP_ACTION_NONE:
		return atomic_load_explicit(&impl->ownership_in_progress,
				memory_order_acquire) ? "finishing" : "idle";
	default:
		return "invalid";
	}
}

static uint64_t next_generation(uint64_t generation)
{
	generation++;
	return generation == 0 ? 1 : generation;
}

static void invalidate_playback_generation(struct impl *impl,
		bool abandon_request)
{
	atomic_store_explicit(&impl->playback_configured, false,
			memory_order_release);
	impl->playback_generation_state = abandon_request ?
			PLAYBACK_GENERATION_NONE : PLAYBACK_GENERATION_WAITING;
	atomic_store_explicit(&impl->playback_installed_generation, 0,
			memory_order_relaxed);
	if (abandon_request)
		impl->playback_requested_generation = 0;
}

static uint32_t slot_token_index(uint64_t token)
{
	return (uint32_t)(token & SLOT_TOKEN_MASK);
}

static uint64_t acquire_slot_token(struct impl *impl, uint32_t index)
{
	uint64_t sequence = atomic_fetch_add_explicit(&impl->next_token, 1,
			memory_order_relaxed);

	if (sequence >= (UINT64_MAX >> SLOT_TOKEN_BITS))
		return SLOT_TOKEN_NONE;
	return (sequence << SLOT_TOKEN_BITS) | index;
}

static void mark_protocol_error(struct impl *impl, const char *operation,
		uint32_t source_line, uint32_t slot, uint32_t expected_state,
		uint32_t observed_state, uint64_t expected_token,
		uint64_t observed_token, int result)
{
	uint32_t expected = FAILURE_NONE;

	atomic_fetch_add_explicit(&impl->output_stats.protocol_errors, 1,
			memory_order_relaxed);
	if (!atomic_compare_exchange_strong_explicit(&impl->fatal_error, &expected,
			FAILURE_RECORDING, memory_order_acq_rel,
			memory_order_relaxed))
		return;
	impl->failure.operation = operation;
	impl->failure.source_line = source_line;
	impl->failure.slot = slot;
	impl->failure.expected_state = expected_state;
	impl->failure.observed_state = observed_state;
	impl->failure.pending_depth = pwao_queue_ring_size(&impl->pending);
	impl->failure.completion_depth = pwao_queue_ring_size(&impl->completions);
	impl->failure.blocked_token = atomic_load_explicit(&impl->blocked_input,
			memory_order_relaxed);
	if (slot < impl->n_capture_buffers) {
		impl->failure.slot_acquisitions = atomic_load_explicit(
			&impl->inputs[slot].acquisitions, memory_order_relaxed);
		impl->failure.slot_returns = atomic_load_explicit(
			&impl->inputs[slot].returns, memory_order_relaxed);
		if (observed_token == SLOT_TOKEN_NONE)
			observed_token = atomic_load_explicit(&impl->inputs[slot].token,
					memory_order_relaxed);
	}
	impl->failure.expected_token = expected_token;
	impl->failure.observed_token = observed_token;
	impl->failure.result = result;
	atomic_store_explicit(&impl->fatal_error, FAILURE_READY,
			memory_order_release);
}

#define MARK_PROTOCOL_ERROR(impl, operation, slot, expected, observed, result) \
	mark_protocol_error((impl), (operation), __LINE__, (slot), (expected), \
			(observed), SLOT_TOKEN_NONE, SLOT_TOKEN_NONE, (result))

#define MARK_TOKEN_PROTOCOL_ERROR(impl, operation, slot, expected, observed, \
		expected_token, observed_token, result) \
	mark_protocol_error((impl), (operation), __LINE__, (slot), (expected), \
			(observed), (expected_token), (observed_token), (result))

static struct input_slot *input_slot_from_buffer(struct pw_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->user_data;
}

static struct output_slot *output_slot_from_buffer(struct pw_buffer *buffer)
{
	return buffer == NULL ? NULL : buffer->user_data;
}

static int return_capture_buffer(struct impl *impl, uint64_t token,
		uint32_t expected_state)
{
	struct input_slot *slot;
	uint32_t expected;
	uint32_t index;
	uint64_t current_token;
	int result;

	if (token == SLOT_TOKEN_NONE)
		return -EINVAL;
	index = slot_token_index(token);
	if (index >= impl->n_capture_buffers)
		return -EINVAL;
	slot = &impl->inputs[index];
	current_token = atomic_load_explicit(&slot->token, memory_order_acquire);
	if (current_token != token)
		return -ESTALE;
	expected = expected_state;
	if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected,
			SLOT_FREE, memory_order_acq_rel, memory_order_relaxed))
		return -EPROTO;
	atomic_store_explicit(&slot->token, SLOT_TOKEN_NONE, memory_order_release);
	if (slot->capture == NULL)
		return -EIO;
	result = pw_stream_queue_buffer(impl->capture, slot->capture);
	if (result >= 0)
		atomic_fetch_add_explicit(&slot->returns, 1, memory_order_relaxed);
	return result;
}

static void drain_completions(struct impl *impl)
{
	uint32_t count;

	for (count = 0; count < impl->n_capture_buffers; count++) {
		uint64_t token;
		uint32_t index = UINT32_MAX;
		int result = pwao_queue_ring_try_pop(&impl->completions, &token);

		if (result == 0)
			break;
		if (result < 0) {
			MARK_PROTOCOL_ERROR(impl, "completion.pop", UINT32_MAX,
					UINT32_MAX, UINT32_MAX, result);
			break;
		}
		if (token == SLOT_TOKEN_NONE ||
				(index = slot_token_index(token)) >= impl->n_capture_buffers) {
			MARK_PROTOCOL_ERROR(impl, "completion.invalid-slot", index,
					UINT32_MAX, UINT32_MAX, -EINVAL);
			break;
		}
		uint32_t state = atomic_load_explicit(&impl->inputs[index].state,
				memory_order_acquire);
		if (state != SLOT_COMPLETING) {
			MARK_PROTOCOL_ERROR(impl, "completion.unexpected-state", index,
					SLOT_COMPLETING, state, -EPROTO);
			break;
		}
		result = return_capture_buffer(impl, token, SLOT_COMPLETING);
		if (result < 0) {
			uint32_t observed = atomic_load_explicit(&impl->inputs[index].state,
					memory_order_acquire);
			uint64_t observed_token = atomic_load_explicit(
					&impl->inputs[index].token, memory_order_acquire);
			MARK_TOKEN_PROTOCOL_ERROR(impl, "completion.return-input", index,
					SLOT_COMPLETING, observed, token, observed_token, result);
			break;
		}
	}
}

static int publish_completion(struct impl *impl, uint64_t token)
{
	struct input_slot *slot;
	uint32_t expected = SLOT_ACTIVE;
	uint32_t index;

	if (token == SLOT_TOKEN_NONE)
		return -EINVAL;
	index = slot_token_index(token);
	if (index >= impl->n_capture_buffers)
		return -EINVAL;
	slot = &impl->inputs[index];
	if (atomic_load_explicit(&slot->token, memory_order_acquire) != token)
		return -ESTALE;
	if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected,
			SLOT_COMPLETING, memory_order_acq_rel,
			memory_order_relaxed))
		return -EPROTO;
	if (pwao_queue_ring_try_push(&impl->completions, token) != 1)
		return -ENOSPC;
	atomic_fetch_add_explicit(&impl->output_stats.completions, 1,
			memory_order_relaxed);
	return 0;
}

static void capture_process(void *data)
{
	struct impl *impl = data;
	uint32_t count;
	uint64_t blocked_input;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire) ||
			ownership_transition_pending(impl))
		return;
	drain_completions(impl);
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0)
		return;

	blocked_input = atomic_load_explicit(&impl->blocked_input,
			memory_order_relaxed);
	if (blocked_input != SLOT_TOKEN_NONE) {
		uint32_t index = slot_token_index(blocked_input);
		struct input_slot *slot;
		uint32_t expected = SLOT_BLOCKED;

		if (index >= impl->n_capture_buffers) {
			MARK_TOKEN_PROTOCOL_ERROR(impl, "capture.blocked-invalid-slot",
					index, SLOT_BLOCKED, UINT32_MAX, blocked_input,
					SLOT_TOKEN_NONE, -EINVAL);
			return;
		}
		slot = &impl->inputs[index];
		if (atomic_load_explicit(&slot->token,
				memory_order_acquire) != blocked_input ||
				!atomic_compare_exchange_strong_explicit(&slot->state, &expected,
					SLOT_PENDING, memory_order_acq_rel,
					memory_order_relaxed)) {
			MARK_TOKEN_PROTOCOL_ERROR(impl, "capture.recover-blocked", index,
					SLOT_BLOCKED, expected, blocked_input,
					atomic_load_explicit(&slot->token,
						memory_order_acquire), -EPROTO);
			return;
		}
		if (pwao_queue_ring_try_push(&impl->pending, blocked_input) != 1) {
			expected = SLOT_PENDING;
			(void)atomic_compare_exchange_strong_explicit(&slot->state, &expected,
					SLOT_BLOCKED, memory_order_acq_rel,
					memory_order_relaxed);
			return;
		}
		atomic_store_explicit(&impl->blocked_input, SLOT_TOKEN_NONE,
				memory_order_release);
	}

	for (count = 0; count < impl->n_capture_buffers; count++) {
		struct pw_buffer *buffer = pw_stream_dequeue_buffer(impl->capture);
		struct input_slot *slot;
		uint32_t expected = SLOT_FREE;
		uint64_t token, released;
		int result;

		if (buffer == NULL)
			break;
		slot = input_slot_from_buffer(buffer);
		if (slot == NULL) {
			MARK_PROTOCOL_ERROR(impl, "capture.missing-slot", UINT32_MAX,
					UINT32_MAX, UINT32_MAX, -EINVAL);
			return;
		}
		if (slot->index >= impl->n_capture_buffers) {
			MARK_PROTOCOL_ERROR(impl, "capture.invalid-slot", slot->index,
					UINT32_MAX, UINT32_MAX, -EINVAL);
			return;
		}
		if (!atomic_compare_exchange_strong_explicit(&slot->state,
				&expected, SLOT_PENDING, memory_order_acq_rel,
				memory_order_relaxed)) {
			MARK_PROTOCOL_ERROR(impl, "capture.acquire", slot->index,
					SLOT_FREE, expected, -EPROTO);
			return;
		}
		token = acquire_slot_token(impl, slot->index);
		if (token == SLOT_TOKEN_NONE) {
			atomic_store_explicit(&slot->state, SLOT_FREE, memory_order_release);
			MARK_PROTOCOL_ERROR(impl, "capture.token-overflow", slot->index,
					SLOT_FREE, SLOT_FREE, -EOVERFLOW);
			return;
		}
		atomic_store_explicit(&slot->token, token, memory_order_release);
		atomic_fetch_add_explicit(&slot->acquisitions, 1,
				memory_order_relaxed);
		atomic_fetch_add_explicit(&impl->input_stats.publications, 1,
				memory_order_relaxed);
		result = pwao_queue_ring_admit(&impl->pending, token,
				impl->overflow, &released);
		switch (result) {
		case PWAO_QUEUE_ADMIT_QUEUED:
			break;
		case PWAO_QUEUE_ADMIT_REPLACED:
			if (released == SLOT_TOKEN_NONE ||
					slot_token_index(released) >= impl->n_capture_buffers) {
				MARK_PROTOCOL_ERROR(impl, "capture.replace-invalid-slot",
						slot_token_index(released), UINT32_MAX,
						UINT32_MAX, -EINVAL);
				return;
			}
			result = return_capture_buffer(impl, released, SLOT_PENDING);
			if (result < 0) {
				uint32_t released_index = slot_token_index(released);
				uint32_t observed = atomic_load_explicit(
						&impl->inputs[released_index].state,
						memory_order_acquire);
				uint64_t observed_token = atomic_load_explicit(
						&impl->inputs[released_index].token,
						memory_order_acquire);
				MARK_TOKEN_PROTOCOL_ERROR(impl,
						"capture.replace-return-input", released_index,
						SLOT_PENDING, observed, released, observed_token, result);
				return;
			}
			atomic_fetch_add_explicit(&impl->input_stats.replacements, 1,
					memory_order_relaxed);
			break;
		case PWAO_QUEUE_ADMIT_DROPPED:
			if (released != token) {
				MARK_PROTOCOL_ERROR(impl, "capture.drop-wrong-slot",
						slot_token_index(released), UINT32_MAX, UINT32_MAX,
						-EPROTO);
				return;
			}
			result = return_capture_buffer(impl, token, SLOT_PENDING);
			if (result < 0) {
				uint32_t observed = atomic_load_explicit(&slot->state,
						memory_order_acquire);
				uint64_t observed_token = atomic_load_explicit(&slot->token,
						memory_order_acquire);
				MARK_TOKEN_PROTOCOL_ERROR(impl, "capture.drop-return-input",
						slot->index, SLOT_PENDING, observed, token,
						observed_token, result);
				return;
			}
			atomic_fetch_add_explicit(
					&impl->input_stats.dropped_arrivals, 1,
					memory_order_relaxed);
			break;
		case PWAO_QUEUE_ADMIT_BACKPRESSURE:
			expected = SLOT_PENDING;
			if (!atomic_compare_exchange_strong_explicit(&slot->state, &expected,
					SLOT_BLOCKED, memory_order_acq_rel,
					memory_order_relaxed)) {
				MARK_TOKEN_PROTOCOL_ERROR(impl, "capture.block", slot->index,
						SLOT_PENDING, expected, token,
						atomic_load_explicit(&slot->token,
							memory_order_acquire), -EPROTO);
				return;
			}
			atomic_store_explicit(&impl->blocked_input, token,
					memory_order_release);
			atomic_fetch_add_explicit(&impl->input_stats.backpressure,
					1, memory_order_relaxed);
			return;
		default:
			MARK_PROTOCOL_ERROR(impl, "capture.admit", slot->index,
					SLOT_PENDING, SLOT_PENDING, result);
			return;
		}
	}
}

static int transfer_buffer(struct impl *impl, uint32_t input_index,
		struct output_slot *output_slot)
{
	struct spa_buffer *input, *output;

	if (input_index >= impl->n_capture_buffers || output_slot == NULL ||
			output_slot->playback == NULL)
		return -EINVAL;
	input = impl->inputs[input_index].capture->buffer;
	output = output_slot->playback->buffer;
	return pwao_queue_buffer_transfer(input, output,
			impl->storage == STORAGE_COPY);
}

static struct output_slot *find_copy_output(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_playback_buffers; i++)
		if (impl->outputs[i].output_available)
			return &impl->outputs[i];
	return NULL;
}

static int recover_backpressure(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	uint64_t blocked_input;

	(void)loop;
	(void)async;
	(void)seq;
	(void)data;
	(void)size;
	if (ownership_transition_pending(impl) ||
			atomic_load_explicit(&impl->destroying, memory_order_acquire)) {
		impl_unref(impl);
		return 0;
	}
	drain_completions(impl);
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0) {
		impl_unref(impl);
		return 0;
	}
	blocked_input = atomic_load_explicit(&impl->blocked_input,
			memory_order_relaxed);
	if (blocked_input != SLOT_TOKEN_NONE) {
		uint32_t index = slot_token_index(blocked_input);
		uint32_t expected = SLOT_BLOCKED;

		if (index >= impl->n_capture_buffers) {
			MARK_TOKEN_PROTOCOL_ERROR(impl,
					"backpressure.blocked-invalid-slot", index,
					SLOT_BLOCKED, UINT32_MAX, blocked_input,
					SLOT_TOKEN_NONE, -EINVAL);
			goto done;
		}
		if (atomic_load_explicit(&impl->inputs[index].token,
					memory_order_acquire) != blocked_input ||
				!atomic_compare_exchange_strong_explicit(
					&impl->inputs[index].state, &expected, SLOT_PENDING,
					memory_order_acq_rel, memory_order_relaxed)) {
			MARK_TOKEN_PROTOCOL_ERROR(impl, "backpressure.recover", index,
					SLOT_BLOCKED, expected, blocked_input,
					atomic_load_explicit(&impl->inputs[index].token,
						memory_order_acquire), -EPROTO);
			goto done;
		}
		if (pwao_queue_ring_try_push(&impl->pending, blocked_input) == 1)
			atomic_store_explicit(&impl->blocked_input, SLOT_TOKEN_NONE,
					memory_order_release);
		else {
			expected = SLOT_PENDING;
			(void)atomic_compare_exchange_strong_explicit(
					&impl->inputs[index].state, &expected, SLOT_BLOCKED,
					memory_order_acq_rel, memory_order_relaxed);
		}
	}
done:
	impl_unref(impl);
	return 0;
}

static void request_backpressure_recovery(struct impl *impl)
{
	struct pw_loop *loop;
	uint64_t blocked_input;
	int result;

	if (impl->overflow != PWAO_QUEUE_OVERFLOW_BACKPRESSURE ||
			ownership_transition_pending(impl) ||
			atomic_load_explicit(&impl->destroying, memory_order_acquire) ||
			impl->capture == NULL ||
			(blocked_input = atomic_load_explicit(&impl->blocked_input,
					memory_order_acquire)) == SLOT_TOKEN_NONE)
		return;
	loop = pw_stream_get_data_loop(impl->capture);
	if (loop == NULL)
		result = -EIO;
	else {
		impl_ref(impl);
		result = pw_loop_invoke(loop, recover_backpressure, 1,
				NULL, 0, false, impl);
		if (result < 0)
			impl_unref(impl);
	}
	if (result < 0)
		MARK_PROTOCOL_ERROR(impl, "backpressure.schedule",
				slot_token_index(blocked_input),
				SLOT_BLOCKED, SLOT_BLOCKED, result);
}

static void reclaim_playback_buffers(struct impl *impl)
{
	struct pw_buffer *buffer;
	int result;

	while ((buffer = pw_stream_dequeue_buffer(impl->playback)) != NULL) {
		struct output_slot *slot = output_slot_from_buffer(buffer);

		if (slot == NULL) {
			MARK_PROTOCOL_ERROR(impl, "playback.missing-slot", UINT32_MAX,
					UINT32_MAX, UINT32_MAX, -EINVAL);
			return;
		}
		if (slot->index >= impl->n_playback_buffers) {
			MARK_PROTOCOL_ERROR(impl, "playback.invalid-slot", slot->index,
					UINT32_MAX, UINT32_MAX, -EINVAL);
			return;
		}
		slot->output_dequeued = true;
		if (slot->output_in_flight) {
			if (impl->storage == STORAGE_LEASE &&
					slot->delivered_input != SLOT_TOKEN_NONE) {
				result = publish_completion(impl, slot->delivered_input);
				if (result < 0) {
					uint32_t delivered_index = slot_token_index(
							slot->delivered_input);
					uint32_t state = slot->delivered_input != SLOT_TOKEN_NONE &&
							delivered_index < impl->n_capture_buffers ?
							atomic_load_explicit(
								&impl->inputs[delivered_index].state,
								memory_order_acquire) : UINT32_MAX;
					MARK_PROTOCOL_ERROR(impl, "playback.complete-lease",
							delivered_index, SLOT_ACTIVE, state, result);
					return;
				}
			}
			slot->output_in_flight = false;
			slot->delivered_input = SLOT_TOKEN_NONE;
			if (atomic_load_explicit(&impl->active_outputs,
					memory_order_relaxed) == 0) {
				MARK_PROTOCOL_ERROR(impl, "playback.active-underflow",
						slot->index, UINT32_MAX, UINT32_MAX, -EPROTO);
				return;
			}
			atomic_fetch_sub_explicit(&impl->active_outputs, 1,
					memory_order_relaxed);
		}
		/* ALLOC_BUFFERS storage is immutable after add_buffer exported it to
		 * the peer.  A returned buffer from an obsolete generation stays
		 * unavailable until PipeWire removes the old pool and installs the
		 * requested generation. */
		slot->output_available =
				atomic_load_explicit(&impl->playback_configured,
					memory_order_acquire) &&
				slot->generation == atomic_load_explicit(
						&impl->playback_installed_generation,
						memory_order_relaxed);
	}
}

/* Native output streams call process only while their free-buffer ring is
 * nonempty. Keep unused output loans in that ring between callbacks. Drain
 * first, then return: return_buffer() inserts at the front, so returning inside
 * reclaim_playback_buffers() would repeatedly dequeue the same buffer. */
static void return_unused_playback_buffers(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < impl->n_playback_buffers; i++) {
		struct output_slot *slot = &impl->outputs[i];
		int result;

		if (!slot->output_dequeued || slot->output_in_flight)
			continue;
		result = pw_stream_return_buffer(impl->playback, slot->playback);
		if (result < 0) {
			/* A failed return leaves the native loan owned by this callback. */
			MARK_PROTOCOL_ERROR(impl, "playback.return-unused", slot->index,
					UINT32_MAX, UINT32_MAX, result);
			continue;
		}
		slot->output_dequeued = false;
		slot->output_available = false;
	}
}

static void playback_process(void *data)
{
	struct impl *impl = data;
	struct output_slot *output_slot;
	uint64_t input_token;
	uint32_t attempt, input_index = UINT32_MAX;
	int result;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire) ||
			ownership_transition_pending(impl))
		return;
	reclaim_playback_buffers(impl);
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0)
		goto done;
	if (!atomic_load_explicit(&impl->playback_configured,
			memory_order_acquire))
		goto done;

	if (atomic_load_explicit(&impl->active_outputs,
			memory_order_relaxed) != 0)
		goto done;
	if (impl->storage == STORAGE_COPY) {
		output_slot = find_copy_output(impl);
		if (output_slot == NULL) {
			atomic_fetch_add_explicit(
					&impl->output_stats.pool_exhaustions, 1,
					memory_order_relaxed);
			goto done;
		}
	} else {
		output_slot = NULL;
	}

	if (impl->storage == STORAGE_LEASE) {
		/* The output lease aliases one specific capture-pool slot.  Inspect the
		 * FIFO head before claiming it so a temporarily unavailable output slot
		 * cannot force that input to the tail and reorder retained frames. */
		for (attempt = 0; attempt < MAX_POOL_BUFFERS; attempt++) {
			result = pwao_queue_ring_try_peek(&impl->pending, &input_token);
			if (result == 0)
				goto done;
			if (result < 0) {
				MARK_PROTOCOL_ERROR(impl, "playback.peek-pending", UINT32_MAX,
						UINT32_MAX, UINT32_MAX, result);
				goto done;
			}
			input_index = slot_token_index(input_token);
			if (input_token == SLOT_TOKEN_NONE ||
					input_index >= impl->n_capture_buffers) {
				MARK_PROTOCOL_ERROR(impl, "playback.pending-invalid-slot",
						input_index, UINT32_MAX, UINT32_MAX, -EINVAL);
				goto done;
			}
			output_slot = &impl->outputs[input_index];
			if (!output_slot->output_available ||
					output_slot->generation != atomic_load_explicit(
						&impl->playback_installed_generation,
						memory_order_relaxed)) {
				atomic_fetch_add_explicit(
						&impl->output_stats.pool_exhaustions, 1,
						memory_order_relaxed);
				goto done;
			}
			result = pwao_queue_ring_try_claim(&impl->pending, input_token);
			if (result == 1)
				break;
			if (result == 0)
				goto done;
			if (result != -EAGAIN) {
				MARK_PROTOCOL_ERROR(impl, "playback.claim-pending",
						input_index, UINT32_MAX, UINT32_MAX, result);
				goto done;
			}
		}
		if (attempt == MAX_POOL_BUFFERS)
			goto done;
	} else {
		result = pwao_queue_ring_try_pop(&impl->pending, &input_token);
		if (result == 0)
			goto done;
		if (result < 0) {
			MARK_PROTOCOL_ERROR(impl, "playback.pop-pending", UINT32_MAX,
					UINT32_MAX, UINT32_MAX, result);
			goto done;
		}
		input_index = slot_token_index(input_token);
		if (input_token == SLOT_TOKEN_NONE ||
				input_index >= impl->n_capture_buffers) {
			MARK_PROTOCOL_ERROR(impl, "playback.pending-invalid-slot",
					input_index, UINT32_MAX, UINT32_MAX, -EINVAL);
			goto done;
		}
	}
	request_backpressure_recovery(impl);
	uint64_t observed_token = atomic_load_explicit(
			&impl->inputs[input_index].token, memory_order_acquire);
	uint32_t state = SLOT_PENDING;
	if (observed_token != input_token ||
			!atomic_compare_exchange_strong_explicit(
				&impl->inputs[input_index].state, &state, SLOT_ACTIVE,
				memory_order_acq_rel, memory_order_relaxed)) {
		MARK_TOKEN_PROTOCOL_ERROR(impl, "playback.acquire-pending", input_index,
				SLOT_PENDING, state, input_token, observed_token, -EPROTO);
		goto done;
	}
	result = transfer_buffer(impl, input_index, output_slot);
	if (result < 0) {
		MARK_PROTOCOL_ERROR(impl, "playback.transfer", input_index,
				SLOT_ACTIVE, SLOT_ACTIVE, result);
		goto done;
	}
	if (impl->storage == STORAGE_COPY) {
		result = publish_completion(impl, input_token);
		if (result < 0) {
			state = atomic_load_explicit(&impl->inputs[input_index].state,
					memory_order_acquire);
			MARK_PROTOCOL_ERROR(impl, "playback.complete-copy", input_index,
					SLOT_ACTIVE, state, result);
			goto done;
		}
	}
	output_slot->output_available = false;
	output_slot->output_in_flight = true;
	output_slot->delivered_input = impl->storage == STORAGE_LEASE ?
			input_token : SLOT_TOKEN_NONE;
	atomic_fetch_add_explicit(&impl->active_outputs, 1,
			memory_order_relaxed);
	atomic_fetch_add_explicit(&impl->output_stats.deliveries, 1,
			memory_order_relaxed);
	result = pw_stream_queue_buffer(impl->playback, output_slot->playback);
	if (result < 0)
		MARK_PROTOCOL_ERROR(impl, "playback.publish", output_slot->index,
				UINT32_MAX, UINT32_MAX, result);
	else
		output_slot->output_dequeued = false;
done:
	return_unused_playback_buffers(impl);
}

static void reset_input_ownership_quiescent(struct impl *impl,
		bool return_capture, bool detach_output_leases)
{
	uint32_t i, limit = return_capture ? impl->n_capture_buffers :
			MAX_POOL_BUFFERS;

	for (i = 0; i < limit; i++) {
		struct input_slot *slot = &impl->inputs[i];
		uint32_t state = atomic_exchange_explicit(&slot->state,
				SLOT_FREE, memory_order_acq_rel);
		atomic_store_explicit(&slot->token, SLOT_TOKEN_NONE,
				memory_order_release);

		if (return_capture && state != SLOT_FREE && slot->capture != NULL &&
				pw_stream_queue_buffer(impl->capture, slot->capture) >= 0)
			atomic_fetch_add_explicit(&slot->returns, 1,
					memory_order_relaxed);
	}
	pwao_queue_ring_reset(&impl->pending);
	pwao_queue_ring_reset(&impl->completions);
	atomic_store_explicit(&impl->blocked_input, SLOT_TOKEN_NONE,
			memory_order_relaxed);
	if (detach_output_leases && impl->storage == STORAGE_LEASE)
		for (i = 0; i < MAX_POOL_BUFFERS; i++) {
			if (impl->outputs[i].output_in_flight)
				impl->outputs[i].delivered_input = SLOT_TOKEN_NONE;
		}
}

static void pause_ownership_quiescent(struct impl *impl)
{
	uint32_t i;

	drain_completions(impl);
	if (atomic_load_explicit(&impl->fatal_error, memory_order_acquire) != 0)
		return;
	for (i = 0; i < impl->n_capture_buffers; i++) {
		struct input_slot *slot = &impl->inputs[i];
		uint32_t state = atomic_load_explicit(&slot->state,
				memory_order_acquire);
		int result;

		if (state == SLOT_FREE ||
				(state == SLOT_ACTIVE && impl->storage == STORAGE_LEASE))
			continue;
		if (state == SLOT_ACTIVE) {
			MARK_PROTOCOL_ERROR(impl, "pause.copy-active", i,
					SLOT_COMPLETING, state, -EPROTO);
			return;
		}
		result = return_capture_buffer(impl,
				atomic_load_explicit(&slot->token, memory_order_acquire),
				state);
		if (result < 0) {
			MARK_PROTOCOL_ERROR(impl, "pause.return-input", i,
					state, atomic_load_explicit(&slot->state,
						memory_order_acquire), result);
			return;
		}
	}
	pwao_queue_ring_reset(&impl->pending);
	pwao_queue_ring_reset(&impl->completions);
	atomic_store_explicit(&impl->blocked_input, SLOT_TOKEN_NONE,
			memory_order_release);
}

static void withdraw_input_pool_quiescent(struct impl *impl)
{
	reset_input_ownership_quiescent(impl, false, true);
}

static int configure_playback(struct impl *impl);
static void maybe_configure_playback(struct impl *impl);
static int ownership_playback_stage(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data);

static int invoke_owned(struct impl *impl, struct pw_loop *loop,
		spa_invoke_func_t function)
{
	int result;

	if (loop == NULL)
		return -EIO;
	impl_ref(impl);
	result = pw_loop_invoke(loop, function, 1, NULL, 0, false, impl);
	if (result < 0)
		impl_unref(impl);
	return result;
}

static int schedule_ownership_playback_stage(struct impl *impl)
{
	if (impl->playback == NULL)
		return -EIO;
	return invoke_owned(impl, pw_stream_get_data_loop(impl->playback),
			ownership_playback_stage);
}

static void cancel_ownership_transition(struct impl *impl)
{
	atomic_store_explicit(&impl->ownership_requested,
			OWNERSHIP_ACTION_NONE, memory_order_release);
	atomic_store_explicit(&impl->ownership_in_progress, false,
			memory_order_release);
}

static void ownership_schedule_failed(struct impl *impl, const char *operation,
		int result)
{
	if (!atomic_load_explicit(&impl->destroying, memory_order_acquire))
		MARK_PROTOCOL_ERROR(impl, operation, UINT32_MAX,
				UINT32_MAX, UINT32_MAX, result);
	cancel_ownership_transition(impl);
}

/* A transition is ordered playback -> capture -> main.  Each asynchronous
 * stage runs after any process callback already executing on that loop.  New
 * process callbacks observe the transition gate and return without touching
 * ownership.  This avoids nesting PipeWire loop locks under the main loop. */
static void finish_ownership_transition(struct impl *impl)
{
	bool expected;
	int result;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire)) {
		cancel_ownership_transition(impl);
		return;
	}
	if (atomic_load_explicit(&impl->ownership_requested,
			memory_order_acquire) != OWNERSHIP_ACTION_NONE) {
		result = schedule_ownership_playback_stage(impl);
		if (result < 0)
			ownership_schedule_failed(impl, "ownership.reschedule", result);
		return;
	}

	atomic_store_explicit(&impl->ownership_in_progress, false,
			memory_order_release);
	/* A requester can publish an action after the check above while it still
	 * sees ownership_in_progress=true.  Recheck after releasing the worker and
	 * claim it on the requester's behalf.  Process callbacks also inspect the
	 * requested action, so this handoff cannot expose a pending transition. */
	if (atomic_load_explicit(&impl->ownership_requested,
			memory_order_acquire) != OWNERSHIP_ACTION_NONE) {
		expected = false;
		if (atomic_compare_exchange_strong_explicit(
				&impl->ownership_in_progress, &expected, true,
				memory_order_acq_rel, memory_order_relaxed)) {
			result = schedule_ownership_playback_stage(impl);
			if (result < 0)
				ownership_schedule_failed(impl,
						"ownership.handoff", result);
		}
		return;
	}
	maybe_configure_playback(impl);
}

static int ownership_main_stage(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;

	(void)loop;
	(void)async;
	(void)seq;
	(void)data;
	(void)size;
	finish_ownership_transition(impl);
	impl_unref(impl);
	return 0;
}

static int ownership_capture_stage(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	enum ownership_action action;
	int result;

	(void)loop;
	(void)async;
	(void)seq;
	(void)data;
	(void)size;
	if (atomic_load_explicit(&impl->destroying, memory_order_acquire)) {
		cancel_ownership_transition(impl);
		impl_unref(impl);
		return 0;
	}
	action = atomic_exchange_explicit(&impl->ownership_requested,
			OWNERSHIP_ACTION_NONE, memory_order_acq_rel);
	if (action == OWNERSHIP_ACTION_WITHDRAW)
		withdraw_input_pool_quiescent(impl);
	else if (action == OWNERSHIP_ACTION_PAUSE)
		pause_ownership_quiescent(impl);
	if (atomic_load_explicit(&impl->destroying, memory_order_acquire)) {
		cancel_ownership_transition(impl);
		impl_unref(impl);
		return 0;
	}
	result = invoke_owned(impl, pw_context_get_main_loop(impl->context),
			ownership_main_stage);
	if (result < 0)
		ownership_schedule_failed(impl, "ownership.main-stage", result);
	impl_unref(impl);
	return 0;
}

static int ownership_playback_stage(struct spa_loop *loop, bool async,
		uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	int result;

	(void)loop;
	(void)async;
	(void)seq;
	(void)data;
	(void)size;
	if (atomic_load_explicit(&impl->destroying, memory_order_acquire)) {
		cancel_ownership_transition(impl);
		impl_unref(impl);
		return 0;
	}
	if (impl->playback != NULL) {
		reclaim_playback_buffers(impl);
		return_unused_playback_buffers(impl);
	}
	result = impl->capture == NULL ? -EIO : invoke_owned(impl,
			pw_stream_get_data_loop(impl->capture), ownership_capture_stage);
	if (result < 0)
		ownership_schedule_failed(impl, "ownership.capture-stage", result);
	impl_unref(impl);
	return 0;
}

static int request_ownership_action(struct impl *impl,
		enum ownership_action action)
{
	uint32_t requested;
	bool expected = false;
	int result;

	if (action == OWNERSHIP_ACTION_NONE ||
			atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return 0;
	requested = atomic_load_explicit(&impl->ownership_requested,
			memory_order_relaxed);
	while (requested < (uint32_t)action &&
			!atomic_compare_exchange_weak_explicit(
				&impl->ownership_requested, &requested, action,
				memory_order_release, memory_order_relaxed))
		;
	if (!atomic_compare_exchange_strong_explicit(
			&impl->ownership_in_progress, &expected, true,
			memory_order_acq_rel, memory_order_relaxed))
		return 0;
	result = schedule_ownership_playback_stage(impl);
	if (result < 0)
		ownership_schedule_failed(impl, "ownership.start", result);
	return result;
}

static int validate_capture_pool(struct impl *impl)
{
	struct spa_buffer *sample;
	uint32_t i, n_datas;

	if (impl->n_capture_buffers < impl->max_buffers + 2u ||
			impl->n_capture_buffers > MAX_POOL_BUFFERS ||
			impl->n_capture_present != impl->n_capture_buffers)
		return -ENOSPC;
	if (impl->inputs[0].capture == NULL)
		return -EINVAL;
	sample = impl->inputs[0].capture->buffer;
	n_datas = sample->n_datas;
	if (n_datas == 0 || n_datas > MAX_DATA_BLOCKS ||
			sample->n_metas > MAX_METAS)
		return -EINVAL;
	for (i = 0; i < sample->n_metas; i++)
		if (sample->metas[i].type == SPA_META_SyncTimeline)
			return -ENOTSUP;
	for (i = 0; i < impl->n_capture_buffers; i++) {
		struct spa_buffer *buffer;
		uint32_t j;

		if (impl->inputs[i].capture == NULL)
			return -EINVAL;
		buffer = impl->inputs[i].capture->buffer;
		if (buffer->n_datas != n_datas ||
				buffer->n_metas != sample->n_metas)
			return -EINVAL;
		for (j = 0; j < sample->n_metas; j++) {
			const struct spa_meta *meta = &sample->metas[j];
			const struct spa_meta *candidate =
					spa_buffer_find_meta(buffer, meta->type);

			if (candidate == NULL || candidate->size != meta->size)
				return -EINVAL;
		}
		for (j = 0; j < n_datas; j++) {
			struct spa_data *data = &buffer->datas[j];

			if (data->maxsize != sample->datas[j].maxsize)
				return -EINVAL;
			if (impl->storage == STORAGE_COPY && data->data == NULL)
				return -ENOTSUP;
			if (impl->storage == STORAGE_LEASE &&
					(data->type >= 32 || data->fd < 0 ||
					 (data->type != SPA_DATA_MemFd &&
					  data->type != SPA_DATA_DmaBuf)))
				return -ENOTSUP;
		}
	}
	return 0;
}

static void capture_add_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct input_slot *slot;
	uint32_t index;

	for (index = 0; index < MAX_POOL_BUFFERS; index++)
		if (impl->inputs[index].capture == NULL)
			break;
	if (index == MAX_POOL_BUFFERS) {
		MARK_PROTOCOL_ERROR(impl, "capture.add-pool-exhausted", index,
				UINT32_MAX, UINT32_MAX, -ENOSPC);
		return;
	}
	slot = &impl->inputs[index];
	if (impl->n_capture_present == 0) {
		impl->capture_pool_withdrawing = false;
		impl->capture_generation = next_generation(impl->capture_generation);
	}
	slot->index = index;
	slot->capture = buffer;
	atomic_store_explicit(&slot->state, SLOT_FREE, memory_order_relaxed);
	atomic_store_explicit(&slot->token, SLOT_TOKEN_NONE, memory_order_relaxed);
	atomic_store_explicit(&slot->acquisitions, 0, memory_order_relaxed);
	atomic_store_explicit(&slot->returns, 0, memory_order_relaxed);
	buffer->user_data = slot;
	impl->n_capture_present++;
	impl->n_capture_buffers = SPA_MAX(impl->n_capture_buffers, index + 1u);
	maybe_configure_playback(impl);
}

static void capture_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct input_slot *slot = input_slot_from_buffer(buffer);

	if (slot == NULL)
		return;
	/* The playback node is a stable graph endpoint. Capture withdrawal starts
	 * a new pool generation, but each old output buffer keeps its duplicated
	 * descriptor until downstream returns it or PipeWire revokes that pool. */
	if (!impl->capture_pool_withdrawing) {
		impl->capture_pool_withdrawing = true;
		invalidate_playback_generation(impl, true);
		(void)request_ownership_action(impl, OWNERSHIP_ACTION_WITHDRAW);
	}
	slot->capture = NULL;
	buffer->user_data = NULL;
	if (impl->n_capture_present == 0) {
		MARK_PROTOCOL_ERROR(impl, "capture.remove-underflow", slot->index,
				UINT32_MAX, UINT32_MAX, -EPROTO);
		return;
	}
	impl->n_capture_present--;
	if (impl->n_capture_present == 0)
		impl->n_capture_buffers = 0;
}

static int prepare_output_slot(struct impl *impl,
		struct output_slot *slot)
{
	struct input_slot *input;
	int result;

	if (slot == NULL || slot->playback == NULL ||
			(impl->storage == STORAGE_LEASE &&
			 slot->index >= impl->n_capture_buffers))
		return -EINVAL;
	input = &impl->inputs[impl->storage == STORAGE_LEASE ? slot->index : 0];
	if (input->capture == NULL)
		return -EINVAL;
	result = pwao_queue_buffer_validate_layout(input->capture->buffer,
			slot->playback->buffer, impl->storage == STORAGE_COPY);
	if (result < 0)
		return result;
	if (impl->storage == STORAGE_LEASE) {
		pwao_queue_buffer_close_fds(slot->owned_fds,
				SPA_N_ELEMENTS(slot->owned_fds));
		result = pwao_queue_buffer_alias(input->capture->buffer,
				slot->playback->buffer, slot->owned_fds,
				SPA_N_ELEMENTS(slot->owned_fds));
		if (result < 0)
			return result;
	}
	return 0;
}

static void maybe_activate_playback_generation(struct impl *impl)
{
	uint32_t i;
	uint64_t generation = impl->playback_requested_generation;

	if (generation == 0 || generation != impl->capture_generation ||
			impl->format == NULL || impl->n_playback_present == 0 ||
			(impl->storage == STORAGE_LEASE &&
			 impl->n_playback_present != impl->n_capture_buffers))
		return;
	for (i = 0; i < impl->n_playback_buffers; i++)
		if (impl->outputs[i].playback == NULL ||
				impl->outputs[i].generation != generation)
			return;
	atomic_store_explicit(&impl->playback_installed_generation, generation,
			memory_order_relaxed);
	impl->playback_generation_state = PLAYBACK_GENERATION_READY;
	/* This release publishes the complete output-slot pool to the playback
	 * process callback.  Slot storage remains immutable until PipeWire
	 * quiesces that callback and emits remove_buffer. */
	atomic_store_explicit(&impl->playback_configured, true,
			memory_order_release);
}

static void playback_add_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct output_slot *slot;
	uint32_t index;

	for (index = 0; index < MAX_POOL_BUFFERS; index++)
		if (impl->outputs[index].playback == NULL)
			break;
	if (index == MAX_POOL_BUFFERS) {
		MARK_PROTOCOL_ERROR(impl, "playback.add-pool-exhausted", index,
				UINT32_MAX, UINT32_MAX, -ENOSPC);
		return;
	}
	slot = &impl->outputs[index];
	slot->playback = buffer;
	slot->output_available = false;
	slot->output_dequeued = false;
	slot->output_in_flight = false;
	slot->generation = 0;
	slot->delivered_input = SLOT_TOKEN_NONE;
	buffer->user_data = slot;
	impl->n_playback_present++;
	impl->n_playback_buffers = SPA_MAX(impl->n_playback_buffers, index + 1u);
	if (impl->playback_requested_generation != 0 &&
			impl->playback_requested_generation == impl->capture_generation &&
			(impl->storage == STORAGE_COPY ||
			 (index < impl->n_capture_buffers &&
			  impl->inputs[index].capture != NULL))) {
		int result = prepare_output_slot(impl, slot);

		if (result < 0) {
			MARK_PROTOCOL_ERROR(impl, "playback.add-layout", index,
					UINT32_MAX, UINT32_MAX, result);
			return;
		}
		slot->generation = impl->playback_requested_generation;
	}
	maybe_activate_playback_generation(impl);
}

static void playback_remove_buffer(void *data, struct pw_buffer *buffer)
{
	struct impl *impl = data;
	struct output_slot *slot = output_slot_from_buffer(buffer);

	if (slot == NULL)
		return;
	/* use_buffers() quiesces the playback process before this callback.  Once
	 * any installed buffer is removed, no member of that pool generation may
	 * be published again.  Keep the request so the following add_buffer
	 * callbacks can qualify the replacement pool. */
	invalidate_playback_generation(impl,
			impl->playback_requested_generation == 0);
	if (slot->output_in_flight) {
		if (impl->storage == STORAGE_LEASE &&
				slot->delivered_input != SLOT_TOKEN_NONE) {
			int result = publish_completion(impl, slot->delivered_input);

			if (result < 0) {
				uint32_t delivered_index = slot_token_index(
						slot->delivered_input);
				uint32_t state = slot->delivered_input != SLOT_TOKEN_NONE &&
						delivered_index < impl->n_capture_buffers ?
						atomic_load_explicit(
							&impl->inputs[delivered_index].state,
							memory_order_acquire) : UINT32_MAX;
				MARK_PROTOCOL_ERROR(impl, "playback.remove-complete-lease",
						delivered_index, SLOT_ACTIVE, state, result);
			}
		}
		if (atomic_load_explicit(&impl->active_outputs,
				memory_order_relaxed) == 0)
			MARK_PROTOCOL_ERROR(impl, "playback.remove-active-underflow",
					slot->index, UINT32_MAX, UINT32_MAX, -EPROTO);
		else
			atomic_fetch_sub_explicit(&impl->active_outputs, 1,
					memory_order_relaxed);
	}
	pwao_queue_buffer_close_fds(slot->owned_fds,
			SPA_N_ELEMENTS(slot->owned_fds));
	slot->playback = NULL;
	slot->output_available = false;
	slot->output_dequeued = false;
	slot->output_in_flight = false;
	slot->generation = 0;
	slot->delivered_input = SLOT_TOKEN_NONE;
	buffer->user_data = NULL;
	if (impl->n_playback_present == 0) {
		MARK_PROTOCOL_ERROR(impl, "playback.remove-underflow", slot->index,
				UINT32_MAX, UINT32_MAX, -EPROTO);
		return;
	}
	impl->n_playback_present--;
	if (impl->n_playback_present == 0)
		impl->n_playback_buffers = 0;
}

static int update_capture_params(struct impl *impl)
{
	uint8_t buffer[1024];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	struct spa_pod_frame acquisition;
	const struct spa_pod *params[3];
	uint32_t n_params = 0;
	uint32_t data_types = impl->storage == STORAGE_LEASE ?
			((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_DmaBuf)) :
			((1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd));

	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers,
			SPA_POD_Int((int32_t)(impl->max_buffers + 2u)),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(data_types));
	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
			SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
			SPA_PARAM_META_size,
			SPA_POD_Int((int32_t)sizeof(struct spa_meta_header)));
	if (impl->media_subtype == SPA_MEDIA_SUBTYPE_ndarray) {
		/* Every fan-out peer must advertise Acquisition, otherwise buffer
		 * negotiation can strip it from the producer's shared pool. */
		spa_pod_builder_push_object(&builder, &acquisition,
				SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
		spa_pod_builder_add(&builder,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Acquisition),
				SPA_PARAM_META_size,
				SPA_POD_Int((int32_t)sizeof(struct spa_meta_acquisition)), 0);
		spa_pod_builder_prop(&builder, SPA_PARAM_META_features,
				SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_int(&builder, SPA_META_FEATURE_ACQUISITION_CURRENT);
		params[n_params++] = spa_pod_builder_pop(&builder, &acquisition);
	}
	return pw_stream_update_params(impl->capture, params, n_params);
}

static void playback_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	(void)data;
	(void)old;
	if (state == PW_STREAM_STATE_ERROR)
		pw_log_log(SPA_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__,
				"queue output entered error state: %s",
				error == NULL ? "unknown error" : error);
}

/* Stream destruction can precede module destruction during Core teardown.
 * Keep buffer listeners installed until disconnection has revoked both pools;
 * playback descriptors must be retired before their capture storage. */
static void quiesce_streams(struct impl *impl)
{
	if (atomic_exchange_explicit(&impl->destroying, true,
			memory_order_acq_rel))
		return;
	if (impl->capture != NULL)
		(void)pw_stream_set_active(impl->capture, false);
	if (impl->playback != NULL)
		(void)pw_stream_set_active(impl->playback, false);
	if (impl->playback != NULL)
		(void)pw_stream_disconnect(impl->playback);
	if (impl->capture != NULL)
		(void)pw_stream_disconnect(impl->capture);
}

static void playback_destroy(void *data)
{
	struct impl *impl = data;

	schedule_destroy(impl);
	quiesce_streams(impl);
	spa_hook_remove(&impl->playback_listener);
	impl->playback = NULL;
}

static void capture_destroy(void *data)
{
	struct impl *impl = data;

	schedule_destroy(impl);
	quiesce_streams(impl);
	spa_hook_remove(&impl->capture_listener);
	impl->capture = NULL;
}

static const struct pw_stream_events playback_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.state_changed = playback_state_changed,
	.process = playback_process,
	.add_buffer = playback_add_buffer,
	.remove_buffer = playback_remove_buffer,
};

static int configure_playback(struct impl *impl)
{
	uint8_t buffer[PARAM_BUFFER_SIZE];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	struct spa_pod_frame acquisition;
	const struct spa_pod *params[2 + MAX_METAS];
	struct spa_buffer *sample;
	uint32_t data_types, i, n_params = 0, size = 0;
	int result;

	if (impl->playback == NULL)
		return -EIO;
	if (impl->capture_generation != 0 &&
			impl->playback_requested_generation == impl->capture_generation)
		return 0;
	if ((result = validate_capture_pool(impl)) < 0)
		return result;
	sample = impl->inputs[0].capture->buffer;
	for (i = 0; i < sample->n_datas; i++) {
		if (sample->datas[i].maxsize > INT32_MAX) {
			result = -EOVERFLOW;
			goto done;
		}
		size = SPA_MAX(size, sample->datas[i].maxsize);
	}
	data_types = impl->storage == STORAGE_LEASE ?
			((1u << SPA_DATA_MemFd) | (1u << SPA_DATA_DmaBuf)) :
			((1u << SPA_DATA_MemPtr) | (1u << SPA_DATA_MemFd));
	params[n_params++] = impl->format;
	params[n_params++] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers,
			SPA_POD_Int((int32_t)impl->n_capture_buffers),
			SPA_PARAM_BUFFERS_blocks,
			SPA_POD_Int((int32_t)sample->n_datas),
			SPA_PARAM_BUFFERS_size, SPA_POD_Int((int32_t)size),
			SPA_PARAM_BUFFERS_dataType,
			SPA_POD_CHOICE_FLAGS_Int(data_types));
	for (i = 0; i < sample->n_metas; i++) {
		if (sample->metas[i].type == SPA_META_Busy ||
		    sample->metas[i].type >= SPA_META_START_features)
			continue;
		if (sample->metas[i].type == SPA_META_Acquisition) {
			spa_pod_builder_push_object(&builder, &acquisition,
					SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
			spa_pod_builder_add(&builder,
					SPA_PARAM_META_type,
					SPA_POD_Id(SPA_META_Acquisition),
					SPA_PARAM_META_size,
					SPA_POD_Int((int32_t)sample->metas[i].size),
					0);
			spa_pod_builder_prop(&builder, SPA_PARAM_META_features,
					SPA_POD_PROP_FLAG_MANDATORY);
			spa_pod_builder_int(&builder,
					SPA_META_FEATURE_ACQUISITION_CURRENT);
			params[n_params++] = spa_pod_builder_pop(&builder,
					&acquisition);
			continue;
		}
		params[n_params++] = spa_pod_builder_add_object(&builder,
				SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
				SPA_PARAM_META_type,
				SPA_POD_Id(sample->metas[i].type),
				SPA_PARAM_META_size,
				SPA_POD_Int((int32_t)sample->metas[i].size));
	}

	/* Updating the buffer contract toggles PipeWire's parameter serial and
	 * requests a new output pool while preserving the node, port, and link.
	 * Existing spa_data descriptors are immutable: readiness is published only
	 * after use_buffers() has removed the previous pool and add_buffer has
	 * installed every descriptor for this capture generation. */
	impl->playback_requested_generation = impl->capture_generation;
	impl->playback_generation_state = PLAYBACK_GENERATION_WAITING;
	atomic_store_explicit(&impl->playback_configured, false,
			memory_order_release);
	result = pw_stream_update_params(impl->playback, params, n_params);
	if (result < 0)
		invalidate_playback_generation(impl, true);
done:
	return result;
}

static void maybe_configure_playback(struct impl *impl)
{
	enum pw_stream_state capture_state;
	int result;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire) ||
			ownership_transition_pending(impl) ||
			impl->capture == NULL || impl->playback == NULL ||
			impl->format == NULL || impl->capture_pool_withdrawing ||
			impl->playback_requested_generation == impl->capture_generation ||
			impl->n_capture_present < impl->max_buffers + 2u)
		return;
	result = configure_playback(impl);
	if (result < 0)
		(void)pw_stream_set_error(impl->capture, result,
				"queue output configuration failed: %s",
				spa_strerror(result));
	else {
		capture_state = pw_stream_get_state(impl->capture, NULL);
		if (capture_state == PW_STREAM_STATE_STREAMING)
			(void)pw_stream_set_active(impl->playback, true);
	}
}

static int setup_playback_endpoint(struct impl *impl)
{
	enum pw_stream_flags flags = PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT | PW_STREAM_FLAG_INACTIVE;

	impl->playback = pw_stream_new(impl->core, "queue output",
			impl->playback_props);
	impl->playback_props = NULL;
	if (impl->playback == NULL)
		return -errno;
	pw_stream_add_listener(impl->playback, &impl->playback_listener,
			&playback_events, impl);
	if (impl->storage == STORAGE_LEASE)
		flags |= PW_STREAM_FLAG_ALLOC_BUFFERS;
	else
		flags |= PW_STREAM_FLAG_MAP_BUFFERS;
	/* Connect without a usable format.  The stable node and port are visible
	 * immediately; configure_playback publishes the capture pool's exact format
	 * and buffer contract once that generation is ready. */
	return pw_stream_connect(impl->playback, PW_DIRECTION_OUTPUT,
			PW_ID_ANY, flags, NULL, 0);
}

static void capture_param_changed(void *data, uint32_t id,
		const struct spa_pod *param)
{
	struct impl *impl = data;
	uint32_t media_type, media_subtype;
	int result;

	if (id != SPA_PARAM_Format)
		return;
	if (param != NULL && atomic_load_explicit(&impl->destroying,
			memory_order_acquire))
		return;
	if (param == NULL) {
		invalidate_playback_generation(impl, true);
		if (!impl->capture_pool_withdrawing)
			(void)request_ownership_action(impl,
					OWNERSHIP_ACTION_WITHDRAW);
		impl->capture_pool_withdrawing = true;
		free(impl->format);
		impl->format = NULL;
		return;
	}
	if (spa_format_parse(param, &media_type, &media_subtype) < 0 ||
			media_type != impl->media_type ||
			media_subtype != impl->media_subtype) {
		(void)pw_stream_set_error(impl->capture, -EINVAL,
				"queue input does not match queue.media");
		return;
	}
	free(impl->format);
	impl->format = spa_pod_copy(param);
	if (impl->format == NULL) {
		(void)pw_stream_set_error(impl->capture, -errno,
				"queue input format copy failed");
		return;
	}
	SPA_POD_OBJECT_ID(impl->format) = SPA_PARAM_EnumFormat;
	if ((result = update_capture_params(impl)) < 0)
		(void)pw_stream_set_error(impl->capture, result,
				"queue input format preparation failed: %s",
				spa_strerror(result));
}

static void stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = data;

	if (atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;
	if (state == PW_STREAM_STATE_UNCONNECTED) {
		schedule_destroy(impl);
		return;
	}
	if (state == PW_STREAM_STATE_ERROR) {
		pw_log_log(SPA_LOG_LEVEL_WARN, __FILE__, __LINE__, __func__,
				"queue input entered error state: %s",
				error == NULL ? "unknown error" : error);
		return;
	}
	if (state == PW_STREAM_STATE_PAUSED && old == PW_STREAM_STATE_STREAMING) {
		(void)request_ownership_action(impl, OWNERSHIP_ACTION_PAUSE);
		return;
	}
	if (state == PW_STREAM_STATE_STREAMING && impl->playback != NULL) {
		maybe_configure_playback(impl);
		(void)pw_stream_set_active(impl->playback, true);
		return;
	}
	if (state == PW_STREAM_STATE_PAUSED)
		maybe_configure_playback(impl);
}

static const struct pw_stream_events capture_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.state_changed = stream_state_changed,
	.param_changed = capture_param_changed,
	.add_buffer = capture_add_buffer,
	.remove_buffer = capture_remove_buffer,
	.process = capture_process,
};

static void core_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	schedule_destroy(impl);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void core_error(void *data, uint32_t id, int seq, int result,
		const char *message)
{
	struct impl *impl = data;

	(void)seq;
	(void)message;
	if (id == PW_ID_CORE && result == -EPIPE)
		schedule_destroy(impl);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void update_stats(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	struct spa_dict_item items[MAX_STATS_ITEMS];
	char values[MAX_STATS_ITEMS][32];
	char message[640];
	char slot[32] = "n/a";
	char blocked_slot[32] = "n/a";
	char expected_token[32] = "n/a";
	char observed_token[32] = "n/a";
	const char *operation = "none";
	const char *expected_state = "n/a";
	const char *observed_state = "n/a";
	uint32_t failure_state;
	uint32_t count = 0;

	(void)expirations;
	/* A Core-owned stream can disappear before deferred module cleanup. */
	if (atomic_load_explicit(&impl->destroying, memory_order_acquire))
		return;

#define ADD_COUNTER(key, field) do { \
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64, \
		atomic_load_explicit(&(field), memory_order_relaxed)); \
	items[count] = SPA_DICT_ITEM_INIT((key), values[count]); \
	count++; \
} while (0)
	ADD_COUNTER("queue.stats.publications", impl->input_stats.publications);
	ADD_COUNTER("queue.stats.replacements", impl->input_stats.replacements);
	ADD_COUNTER("queue.stats.dropped-arrivals",
			impl->input_stats.dropped_arrivals);
	ADD_COUNTER("queue.stats.backpressure", impl->input_stats.backpressure);
	ADD_COUNTER("queue.stats.deliveries", impl->output_stats.deliveries);
	ADD_COUNTER("queue.stats.completions", impl->output_stats.completions);
	ADD_COUNTER("queue.stats.pool-exhaustions",
			impl->output_stats.pool_exhaustions);
	ADD_COUNTER("queue.stats.protocol-errors",
			impl->output_stats.protocol_errors);
#undef ADD_COUNTER
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			pwao_queue_ring_size(&impl->pending));
	items[count] = SPA_DICT_ITEM_INIT("queue.state.pending-depth",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			pwao_queue_ring_size(&impl->completions));
	items[count] = SPA_DICT_ITEM_INIT("queue.state.completion-depth",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			atomic_load_explicit(&impl->active_outputs,
				memory_order_relaxed));
	items[count] = SPA_DICT_ITEM_INIT("queue.state.active-outputs",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			impl->n_capture_present);
	items[count] = SPA_DICT_ITEM_INIT("queue.state.capture-buffers",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			impl->n_playback_present);
	items[count] = SPA_DICT_ITEM_INIT("queue.state.playback-buffers",
			values[count]);
	count++;
	items[count++] = SPA_DICT_ITEM_INIT("queue.state.configured",
			atomic_load_explicit(&impl->playback_configured,
				memory_order_acquire) ? "true" : "false");
	items[count++] = SPA_DICT_ITEM_INIT("queue.state.input-format",
			impl->format != NULL ? "negotiated" : "none");
	items[count++] = SPA_DICT_ITEM_INIT("queue.state.input-stream",
			pw_stream_state_as_string(pw_stream_get_state(impl->capture, NULL)));
	items[count++] = SPA_DICT_ITEM_INIT("queue.state.output-stream",
			pw_stream_state_as_string(pw_stream_get_state(impl->playback, NULL)));
	items[count++] = SPA_DICT_ITEM_INIT("queue.state.ownership-transition",
			ownership_transition_name(impl));
	items[count++] = SPA_DICT_ITEM_INIT("queue.state.output-generation",
			playback_generation_state_name(
				impl->playback_generation_state));
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64,
			impl->capture_generation);
	items[count] = SPA_DICT_ITEM_INIT("queue.state.capture-generation",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64,
			impl->playback_requested_generation);
	items[count] = SPA_DICT_ITEM_INIT("queue.state.requested-generation",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64,
			atomic_load_explicit(&impl->playback_installed_generation,
				memory_order_relaxed));
	items[count] = SPA_DICT_ITEM_INIT("queue.state.installed-generation",
			values[count]);
	count++;
	failure_state = atomic_load_explicit(&impl->fatal_error,
			memory_order_acquire);
	if (failure_state == FAILURE_READY) {
		operation = impl->failure.operation;
		expected_state = slot_state_name(impl->failure.expected_state);
		observed_state = slot_state_name(impl->failure.observed_state);
	}
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.operation", operation);
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			failure_state == FAILURE_READY ? impl->failure.source_line : 0);
	items[count] = SPA_DICT_ITEM_INIT("queue.error.source-line", values[count]);
	count++;
	if (failure_state == FAILURE_READY && impl->failure.slot != UINT32_MAX)
		(void)snprintf(slot, sizeof(slot), "%u", impl->failure.slot);
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.slot", slot);
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.expected-state",
			expected_state);
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.observed-state",
			observed_state);
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			failure_state == FAILURE_READY ?
			impl->failure.pending_depth : 0);
	items[count] = SPA_DICT_ITEM_INIT("queue.error.pending-depth",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%u",
			failure_state == FAILURE_READY ?
			impl->failure.completion_depth : 0);
	items[count] = SPA_DICT_ITEM_INIT("queue.error.completion-depth",
			values[count]);
	count++;
	if (failure_state == FAILURE_READY &&
			impl->failure.blocked_token != SLOT_TOKEN_NONE)
		(void)snprintf(blocked_slot, sizeof(blocked_slot), "%u",
				slot_token_index(impl->failure.blocked_token));
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.blocked-slot",
			blocked_slot);
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64,
			failure_state == FAILURE_READY ?
			impl->failure.slot_acquisitions : 0);
	items[count] = SPA_DICT_ITEM_INIT("queue.error.slot-acquisitions",
			values[count]);
	count++;
	(void)snprintf(values[count], sizeof(values[count]), "%" PRIu64,
			failure_state == FAILURE_READY ? impl->failure.slot_returns : 0);
	items[count] = SPA_DICT_ITEM_INIT("queue.error.slot-returns",
			values[count]);
	count++;
	if (failure_state == FAILURE_READY &&
			impl->failure.expected_token != SLOT_TOKEN_NONE)
		(void)snprintf(expected_token, sizeof(expected_token), "%" PRIu64,
				impl->failure.expected_token);
	if (failure_state == FAILURE_READY &&
			impl->failure.observed_token != SLOT_TOKEN_NONE)
		(void)snprintf(observed_token, sizeof(observed_token), "%" PRIu64,
				impl->failure.observed_token);
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.expected-token",
			expected_token);
	items[count++] = SPA_DICT_ITEM_INIT("queue.error.observed-token",
			observed_token);
	(void)snprintf(values[count], sizeof(values[count]), "%d",
			failure_state == FAILURE_READY ? impl->failure.result : 0);
	items[count] = SPA_DICT_ITEM_INIT("queue.error.result", values[count]);
	count++;
	pw_impl_module_update_properties(impl->module,
			&SPA_DICT_INIT(items, count));
	/* Module properties are local when this module is loaded by a connected
	 * manager client. Mirror diagnostics onto both exported stream nodes so
	 * graph tools can inspect a live queue without access to that client. */
	if (impl->capture != NULL)
		(void)pw_stream_update_properties(impl->capture,
				&SPA_DICT_INIT(items, count));
	if (impl->playback != NULL)
		(void)pw_stream_update_properties(impl->playback,
				&SPA_DICT_INIT(items, count));
	if (failure_state == FAILURE_READY && !impl->failure_reported) {
		impl->failure_reported = true;
		(void)snprintf(message, sizeof(message),
				"queue ownership protocol failed: operation=%s "
				"source-line=%u slot=%s expected-state=%s "
				"observed-state=%s pending-depth=%u completion-depth=%u "
				"blocked-slot=%s slot-acquisitions=%" PRIu64 " "
				"slot-returns=%" PRIu64 " expected-token=%s "
				"observed-token=%s result=%d (%s)",
				operation, impl->failure.source_line, slot,
				expected_state, observed_state, impl->failure.pending_depth,
				impl->failure.completion_depth, blocked_slot,
				impl->failure.slot_acquisitions,
				impl->failure.slot_returns,
				expected_token, observed_token,
				impl->failure.result,
				spa_strerror(impl->failure.result));
		pw_log_log(SPA_LOG_LEVEL_ERROR, __FILE__, __LINE__, __func__,
				"%s", message);
		if (impl->capture != NULL)
			(void)pw_stream_set_error(impl->capture, -EPROTO,
					"%s", message);
		if (impl->playback != NULL)
			(void)pw_stream_set_error(impl->playback, -EPROTO,
					"%s", message);
	}
}

static void impl_free(struct impl *impl)
{
	uint32_t i;

	for (i = 0; i < MAX_POOL_BUFFERS; i++)
		pwao_queue_buffer_close_fds(impl->outputs[i].owned_fds,
				SPA_N_ELEMENTS(impl->outputs[i].owned_fds));
	free(impl->format);
	pw_properties_free(impl->capture_props);
	pw_properties_free(impl->playback_props);
	free(impl);
}

static void impl_destroy(struct impl *impl)
{
	bool expected = false;

	if (!atomic_compare_exchange_strong_explicit(&impl->cleanup_started,
			&expected, true, memory_order_acq_rel, memory_order_relaxed))
		return;
	atomic_store_explicit(&impl->destroy_scheduled, true,
			memory_order_release);
	if (impl->stats_timer != NULL) {
		pw_loop_destroy_source(pw_context_get_main_loop(impl->context),
				impl->stats_timer);
		impl->stats_timer = NULL;
	}
	quiesce_streams(impl);
	if (impl->playback != NULL) {
		pw_stream_destroy(impl->playback);
		impl->playback = NULL;
	}
	if (impl->capture != NULL) {
		pw_stream_destroy(impl->capture);
		impl->capture = NULL;
	}
	if (impl->core != NULL) {
		struct pw_core *core = impl->core;

		spa_hook_remove(&impl->core_listener);
		spa_hook_remove(&impl->core_proxy_listener);
		impl->core = NULL;
		if (impl->disconnect_core)
			pw_core_disconnect(core);
	}
	impl_unref(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int parse_options(struct impl *impl, const struct pw_properties *props)
{
	const char *value;

	value = pw_properties_get(props, "queue.max-buffers");
	if (value == NULL || !spa_atou32(value, &impl->max_buffers, 10) ||
			impl->max_buffers == 0 ||
			impl->max_buffers > MAX_QUEUE_BUFFERS)
		return -EINVAL;
	value = pw_properties_get(props, "queue.overflow");
	if (value != NULL && spa_streq(value, "backpressure"))
		impl->overflow = PWAO_QUEUE_OVERFLOW_BACKPRESSURE;
	else if (spa_streq(value, "drop-oldest"))
		impl->overflow = PWAO_QUEUE_OVERFLOW_DROP_OLDEST;
	else if (spa_streq(value, "drop-newest"))
		impl->overflow = PWAO_QUEUE_OVERFLOW_DROP_NEWEST;
	else
		return -EINVAL;
	value = pw_properties_get(props, "queue.storage");
	if (value != NULL && spa_streq(value, "copy"))
		impl->storage = STORAGE_COPY;
	else if (spa_streq(value, "lease"))
		impl->storage = STORAGE_LEASE;
	else
		return -EINVAL;
	value = pw_properties_get(props, "queue.media");
	if (value == NULL || spa_streq(value, "application/ndarray")) {
		impl->media_type = SPA_MEDIA_TYPE_application;
		impl->media_subtype = SPA_MEDIA_SUBTYPE_ndarray;
	} else if (spa_streq(value, "video/raw")) {
		impl->media_type = SPA_MEDIA_TYPE_video;
		impl->media_subtype = SPA_MEDIA_SUBTYPE_raw;
	} else {
		return -EINVAL;
	}
	return 0;
}

static int setup_properties(struct impl *impl, struct pw_properties *props,
		uint32_t id)
{
	struct spa_error_location location;
	const char *value, *name;
	uint32_t pid = (uint32_t)getpid();
	int result;

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL)
		return -errno;
	if ((value = pw_properties_get(props, "capture.props")) != NULL &&
			(result = pw_properties_update_string_checked(impl->capture_props,
				value, strlen(value), &location)) < 0)
		return result;
	if ((value = pw_properties_get(props, "playback.props")) != NULL &&
			(result = pw_properties_update_string_checked(impl->playback_props,
				value, strlen(value), &location)) < 0)
		return result;
	name = pw_properties_get(props, PW_KEY_NODE_NAME);
	if (name == NULL)
		name = "queue";
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_NAME,
				"input.%s-%u-%u", name, pid, id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_NAME,
				"output.%s-%u-%u", name, pid, id);
	pw_properties_setf(impl->capture_props, PWAO_QUEUE_ID_PROPERTY,
			"%u-%u", pid, id);
	pw_properties_setf(impl->playback_props, PWAO_QUEUE_ID_PROPERTY,
			"%u-%u", pid, id);
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_GROUP,
				"queue.capture-%u-%u", pid, id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_GROUP,
				"queue.playback-%u-%u", pid, id);
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_LINK_GROUP,
				"queue.capture-link-%u-%u", pid, id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_LINK_GROUP) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_LINK_GROUP,
				"queue.playback-link-%u-%u", pid, id);
	if (pw_properties_get(impl->capture_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_MEDIA_CLASS,
				impl->media_type == SPA_MEDIA_TYPE_video ?
				"Video/Sink" : "Data/Sink");
	if (pw_properties_get(impl->playback_props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_MEDIA_CLASS,
				impl->media_type == SPA_MEDIA_TYPE_video ?
				"Video/Source" : "Data/Source");
	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(impl->capture_props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(impl->playback_props, PW_KEY_NODE_VIRTUAL, "true");
	return 0;
}

static int setup_capture(struct impl *impl)
{
	uint8_t buffer[512];
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer,
			sizeof(buffer));
	const struct spa_pod *params[1];

	params[0] = spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(impl->media_type),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(impl->media_subtype));
	impl->capture = pw_stream_new(impl->core, "queue input",
			impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;
	pw_stream_add_listener(impl->capture, &impl->capture_listener,
			&capture_events, impl);
	return pw_stream_connect(impl->capture, PW_DIRECTION_INPUT, PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
			PW_STREAM_FLAG_NO_CONVERT,
			params, SPA_N_ELEMENTS(params));
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct spa_error_location location;
	struct pw_properties *props = NULL;
	struct impl *impl = NULL;
	const char *remote;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	uint32_t i, j;
	int result;
	struct timespec interval = { .tv_sec = 1, .tv_nsec = 0 };

	result = posix_memalign((void **)&impl, SPA_CACHE_LINE_SIZE,
			sizeof(*impl));
	if (result != 0)
		return -result;
	memset(impl, 0, sizeof(*impl));
	impl->context = context;
	impl->module = module;
	atomic_init(&impl->ownership_requested, OWNERSHIP_ACTION_NONE);
	atomic_init(&impl->ownership_in_progress, false);
	atomic_init(&impl->refs, 1);
	atomic_init(&impl->blocked_input, SLOT_TOKEN_NONE);
	atomic_init(&impl->next_token, 0);
	atomic_init(&impl->active_outputs, 0);
	atomic_init(&impl->playback_configured, false);
	atomic_init(&impl->playback_installed_generation, 0);
	atomic_init(&impl->fatal_error, FAILURE_NONE);
	atomic_init(&impl->destroy_scheduled, false);
	atomic_init(&impl->destroying, false);
	atomic_init(&impl->cleanup_started, false);
	atomic_init(&impl->input_stats.publications, 0);
	atomic_init(&impl->input_stats.replacements, 0);
	atomic_init(&impl->input_stats.dropped_arrivals, 0);
	atomic_init(&impl->input_stats.backpressure, 0);
	atomic_init(&impl->output_stats.deliveries, 0);
	atomic_init(&impl->output_stats.completions, 0);
	atomic_init(&impl->output_stats.pool_exhaustions, 0);
	atomic_init(&impl->output_stats.protocol_errors, 0);
	for (i = 0; i < MAX_POOL_BUFFERS; i++) {
		impl->inputs[i].index = i;
		atomic_init(&impl->inputs[i].state, SLOT_FREE);
		atomic_init(&impl->inputs[i].token, SLOT_TOKEN_NONE);
		atomic_init(&impl->inputs[i].acquisitions, 0);
		atomic_init(&impl->inputs[i].returns, 0);
		impl->outputs[i].index = i;
		impl->outputs[i].delivered_input = SLOT_TOKEN_NONE;
		for (j = 0; j < MAX_DATA_BLOCKS; j++)
			impl->outputs[i].owned_fds[j] = -1;
	}
	props = args == NULL ? pw_properties_new(NULL, NULL) :
			pw_properties_new_string_checked(args, strlen(args), &location);
	if (props == NULL) {
		result = -errno;
		goto error;
	}
	if ((result = parse_options(impl, props)) < 0 ||
			(result = setup_properties(impl, props, id)) < 0 ||
			(result = pwao_queue_ring_init(&impl->pending,
				impl->max_buffers)) < 0 ||
			(result = pwao_queue_ring_init(&impl->completions,
				MAX_POOL_BUFFERS)) < 0)
		goto error;

	impl->core = pw_context_get_object(context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		const struct pw_properties *context_props =
				pw_context_get_properties(context);
		const char *daemon = pw_properties_get(context_props,
				PW_KEY_CORE_DAEMON);

		remote = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		if ((daemon != NULL && spa_atob(daemon)) ||
				(remote != NULL && spa_streq(remote, "internal")))
			impl->core = pw_context_connect_self(context, NULL, 0);
		else
			impl->core = pw_context_connect(context,
					pw_properties_new(PW_KEY_REMOTE_NAME, remote, NULL), 0);
		impl->disconnect_core = true;
	}
	if (impl->core == NULL) {
		result = -errno;
		goto error;
	}
	pw_proxy_add_listener((struct pw_proxy *)impl->core,
			&impl->core_proxy_listener, &core_proxy_events, impl);
	pw_core_add_listener(impl->core, &impl->core_listener,
			&core_events, impl);
	if ((result = setup_playback_endpoint(impl)) < 0 ||
			(result = setup_capture(impl)) < 0)
		goto error;
	pw_impl_module_add_listener(module, &impl->module_listener,
			&module_events, impl);
	pw_impl_module_update_properties(module,
			&SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);
	update_stats(impl, 0);
	impl->stats_timer = pw_loop_add_timer(pw_context_get_main_loop(context),
			update_stats, impl);
	if (impl->stats_timer == NULL) {
		result = -errno;
		goto error_listener;
	}
	pw_loop_update_timer(pw_context_get_main_loop(context), impl->stats_timer,
			&interval, &interval, false);
	pw_properties_free(props);
	return 0;

error_listener:
	spa_hook_remove(&impl->module_listener);
error:
	pw_properties_free(props);
	impl_destroy(impl);
	return result;
}
