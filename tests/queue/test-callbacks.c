/* SPDX-License-Identifier: MIT */

/* Run the real queue callbacks against two deterministic stream endpoints.
 * The live and remote suites exercise the same callbacks in a PipeWire graph. */
#include "module-queue.c"

struct stream_fixture {
	struct pw_buffer *dequeue[2];
	size_t next, count;
	struct pw_buffer *returned, *queued;
	int queue_result, return_result;
	unsigned int errors;
};

static struct stream_fixture capture, playback;
static struct pw_properties *module_properties;
static struct pw_loop *capture_loop;

static struct pw_stream *stream_handle(struct stream_fixture *stream)
{
	return (struct pw_stream *)(void *)stream;
}

static struct stream_fixture *stream_fixture(struct pw_stream *stream)
{
	spa_assert_se(stream == stream_handle(&capture) || stream == stream_handle(&playback));
	return (struct stream_fixture *)(void *)stream;
}

struct pw_buffer *__wrap_pw_stream_dequeue_buffer(struct pw_stream *stream)
{
	struct stream_fixture *fixture = stream_fixture(stream);
	if (fixture->next == fixture->count)
		return NULL;
	return fixture->dequeue[fixture->next++];
}

int __wrap_pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream_fixture *fixture = stream_fixture(stream);
	fixture->queued = buffer;
	return fixture->queue_result;
}

int __wrap_pw_stream_return_buffer(struct pw_stream *stream, struct pw_buffer *buffer)
{
	struct stream_fixture *fixture = stream_fixture(stream);
	fixture->returned = buffer;
	return fixture->return_result;
}

struct pw_loop *__wrap_pw_stream_get_data_loop(struct pw_stream *stream)
{
	spa_assert_se(stream == stream_handle(&capture));
	return capture_loop;
}

uint32_t __wrap_pw_stream_get_node_id(struct pw_stream *stream)
{
	spa_assert_se(stream == stream_handle(&capture));
	return 1;
}

enum pw_stream_state __wrap_pw_stream_get_state(struct pw_stream *stream, const char **error)
{
	(void)stream_fixture(stream);
	spa_assert_se(error == NULL);
	return PW_STREAM_STATE_PAUSED;
}

int __wrap_pw_stream_update_properties(struct pw_stream *stream, const struct spa_dict *dict)
{
	(void)stream_fixture(stream);
	spa_assert_se(spa_dict_lookup(dict, "queue.error.operation") != NULL);
	return 0;
}

int __wrap_pw_stream_set_error(struct pw_stream *stream, int result, const char *format, ...)
{
	spa_assert_se(result == -EPROTO && spa_streq(format, "%s"));
	stream_fixture(stream)->errors++;
	return result;
}

int __wrap_pw_impl_module_update_properties(struct pw_impl_module *module, const struct spa_dict *dict)
{
	spa_assert_se(module == NULL);
	return pw_properties_update(module_properties, dict);
}

struct fixture {
	struct impl queue;
	struct pw_buffer input[2], output[2];
	struct spa_buffer input_buffer[2], output_buffer[2];
};

static void prepare(struct fixture *fixture)
{
	struct impl *queue = &fixture->queue;

	*fixture = (struct fixture) { 0 };
	capture = (struct stream_fixture) { 0 };
	playback = (struct stream_fixture) { 0 };
	queue->capture = stream_handle(&capture);
	queue->playback = stream_handle(&playback);
	queue->overflow = PWAO_QUEUE_OVERFLOW_BACKPRESSURE;
	queue->n_capture_buffers = queue->n_playback_buffers = 2;
	queue->n_capture_present = queue->n_playback_present = 2;
	queue->refs = 1;
	queue->blocked_input = SLOT_TOKEN_NONE;
	spa_assert_se(pwao_queue_ring_init(&queue->pending, 1) == 0);
	spa_assert_se(pwao_queue_ring_init(&queue->completions, 1) == 0);
	for (uint32_t i = 0; i < 2; i++) {
		fixture->input[i].user_data = &queue->inputs[i];
		fixture->input[i].buffer = &fixture->input_buffer[i];
		queue->inputs[i].capture = &fixture->input[i];
		queue->inputs[i].index = i;
		queue->inputs[i].token = SLOT_TOKEN_NONE;
		fixture->output[i].user_data = &queue->outputs[i];
		fixture->output[i].buffer = &fixture->output_buffer[i];
		queue->outputs[i].playback = &fixture->output[i];
		queue->outputs[i].index = i;
		queue->outputs[i].delivered_input = SLOT_TOKEN_NONE;
		for (size_t block = 0; block < MAX_DATA_BLOCKS; block++)
			queue->outputs[i].owned_fds[block] = -1;
	}
}

static void check_failure(const struct impl *queue, const char *operation, int result)
{
	spa_assert_se(queue->fatal_error == FAILURE_READY);
	spa_assert_se(spa_streq(queue->failure.operation, operation));
	spa_assert_se(queue->failure.result == result);
}

static void offer_capture(struct pw_buffer *first, struct pw_buffer *second)
{
	capture.next = 0;
	capture.count = second == NULL ? 1 : 2;
	capture.dequeue[0] = first;
	capture.dequeue[1] = second;
}

static void offer_playback(struct pw_buffer *buffer)
{
	playback.next = 0;
	playback.count = 1;
	playback.dequeue[0] = buffer;
}

static void test_blocked_retry(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;
	uint64_t token;

	for (unsigned int retry_in_loop = 0; retry_in_loop < 2; retry_in_loop++) {
		prepare(&fixture);
		offer_capture(&fixture.input[0], &fixture.input[1]);
		capture_process(queue);
		uint64_t blocked = queue->blocked_input;
		spa_assert_se(blocked != SLOT_TOKEN_NONE && queue->inputs[1].state == SLOT_BLOCKED);
		spa_assert_se(queue->input_stats.backpressure == 1);
		for (unsigned int loop = 0; loop < 2; loop++) {
			if (retry_in_loop) {
				queue->refs++;
				spa_assert_se(recover_backpressure(NULL, true, 0, NULL, 0, queue) == 0);
				spa_assert_se(queue->refs == 1);
			} else {
				capture_process(queue);
			}
			if (loop == 0) {
				spa_assert_se(queue->blocked_input == blocked && queue->inputs[1].state == SLOT_BLOCKED);
				spa_assert_se(pwao_queue_ring_try_pop(&queue->pending, &token) == 1);
				spa_assert_se(slot_token_index(token) == 0);
			}
		}
		spa_assert_se(queue->fatal_error == FAILURE_NONE);
		spa_assert_se(queue->blocked_input == SLOT_TOKEN_NONE && queue->inputs[1].state == SLOT_PENDING);
		spa_assert_se(pwao_queue_ring_try_pop(&queue->pending, &token) == 1 && token == blocked);
	}
}

static void test_capture_faults(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;

	prepare(&fixture);
	fixture.input[0].user_data = NULL;
	offer_capture(&fixture.input[0], NULL);
	capture_process(queue);
	check_failure(queue, "capture.missing-slot", -EINVAL);

	prepare(&fixture);
	queue->inputs[0].index = 2;
	offer_capture(&fixture.input[0], NULL);
	capture_process(queue);
	check_failure(queue, "capture.invalid-slot", -EINVAL);

	prepare(&fixture);
	offer_capture(&fixture.input[0], &fixture.input[0]);
	capture_process(queue);
	check_failure(queue, "capture.acquire", -EPROTO);
	spa_assert_se(queue->failure.slot_acquisitions == 1 && queue->failure.slot_returns == 0);

	prepare(&fixture);
	queue->next_token = UINT64_MAX >> SLOT_TOKEN_BITS;
	offer_capture(&fixture.input[0], NULL);
	capture_process(queue);
	check_failure(queue, "capture.token-overflow", -EOVERFLOW);
	spa_assert_se(queue->inputs[0].state == SLOT_FREE);

	const enum pwao_queue_overflow policies[] = {
		PWAO_QUEUE_OVERFLOW_DROP_OLDEST, PWAO_QUEUE_OVERFLOW_DROP_NEWEST,
	};
	for (size_t i = 0; i < SPA_N_ELEMENTS(policies); i++) {
		prepare(&fixture);
		queue->overflow = policies[i];
		capture.queue_result = -EIO;
		offer_capture(&fixture.input[0], &fixture.input[1]);
		capture_process(queue);
		check_failure(queue, i == 0 ? "capture.replace-return-input" : "capture.drop-return-input", -EIO);
		spa_assert_se(capture.queued == &fixture.input[i]);
		spa_assert_se(queue->inputs[i].returns == 0);
	}
}

static void test_completion_faults(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;

	prepare(&fixture);
	spa_assert_se(return_capture_buffer(queue, SLOT_TOKEN_NONE, SLOT_ACTIVE) == -EINVAL);
	spa_assert_se(return_capture_buffer(queue, 3, SLOT_ACTIVE) == -EINVAL);
	spa_assert_se(return_capture_buffer(queue, 0, SLOT_ACTIVE) == -ESTALE);
	spa_assert_se(publish_completion(queue, SLOT_TOKEN_NONE) == -EINVAL);
	spa_assert_se(publish_completion(queue, 3) == -EINVAL);
	spa_assert_se(publish_completion(queue, 0) == -ESTALE);
	queue->inputs[0].token = 0;
	spa_assert_se(return_capture_buffer(queue, 0, SLOT_ACTIVE) == -EPROTO);
	spa_assert_se(publish_completion(queue, 0) == -EPROTO);
	spa_assert_se(pwao_queue_ring_try_push(&queue->completions, 0) == 1);
	drain_completions(queue);
	check_failure(queue, "completion.unexpected-state", -EPROTO);

	prepare(&fixture);
	spa_assert_se(pwao_queue_ring_try_push(&queue->completions, 3) == 1);
	drain_completions(queue);
	check_failure(queue, "completion.invalid-slot", -EINVAL);

	prepare(&fixture);
	queue->inputs[0].state = SLOT_COMPLETING;
	queue->inputs[0].token = 0;
	queue->inputs[0].capture = NULL;
	spa_assert_se(pwao_queue_ring_try_push(&queue->completions, 0) == 1);
	drain_completions(queue);
	check_failure(queue, "completion.return-input", -EIO);

	prepare(&fixture);
	queue->inputs[0].state = SLOT_ACTIVE;
	queue->inputs[0].token = 0;
	spa_assert_se(pwao_queue_ring_try_push(&queue->completions, 1) == 1);
	spa_assert_se(publish_completion(queue, 0) == -ENOSPC);
	spa_assert_se(queue->inputs[0].state == SLOT_COMPLETING);
	spa_assert_se(queue->output_stats.completions == 0);
}

static void test_playback_faults(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;

	prepare(&fixture);
	playback_process(queue);
	spa_assert_se(queue->output_stats.deliveries == 0);
	queue->playback_configured = true;
	playback_process(queue);
	spa_assert_se(queue->output_stats.pool_exhaustions == 1);

	prepare(&fixture);
	fixture.output[0].user_data = NULL;
	offer_playback(&fixture.output[0]);
	playback_process(queue);
	check_failure(queue, "playback.missing-slot", -EINVAL);

	prepare(&fixture);
	queue->outputs[0].index = 2;
	offer_playback(&fixture.output[0]);
	playback_process(queue);
	check_failure(queue, "playback.invalid-slot", -EINVAL);

	prepare(&fixture);
	queue->outputs[0].output_in_flight = true;
	offer_playback(&fixture.output[0]);
	playback_process(queue);
	check_failure(queue, "playback.active-underflow", -EPROTO);

	prepare(&fixture);
	queue->storage = STORAGE_LEASE;
	queue->outputs[0].output_in_flight = true;
	queue->outputs[0].delivered_input = 0;
	queue->active_outputs = 1;
	offer_playback(&fixture.output[0]);
	playback_process(queue);
	check_failure(queue, "playback.complete-lease", -ESTALE);
	spa_assert_se(queue->active_outputs == 1);

	prepare(&fixture);
	playback.return_result = -EIO;
	offer_playback(&fixture.output[0]);
	playback_process(queue);
	check_failure(queue, "playback.return-unused", -EIO);
	spa_assert_se(playback.returned == &fixture.output[0] && queue->outputs[0].output_dequeued);

	prepare(&fixture);
	queue->playback_configured = true;
	queue->storage = STORAGE_LEASE;
	spa_assert_se(pwao_queue_ring_try_push(&queue->pending, 0) == 1);
	playback_process(queue);
	uint64_t head;
	spa_assert_se(pwao_queue_ring_try_peek(&queue->pending, &head) == 1 && head == 0);
	spa_assert_se(queue->output_stats.pool_exhaustions == 1);
}

static void test_playback_publish_failures(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;
	uint8_t in = 37, out;
	struct spa_chunk input_chunk = { .size = sizeof(in), .stride = 1 }, output_chunk;
	struct spa_data input = { .type = SPA_DATA_MemPtr, .data = &in, .maxsize = sizeof(in), .chunk = &input_chunk };
	struct spa_data output = { .type = SPA_DATA_MemPtr, .data = &out, .maxsize = sizeof(out), .chunk = &output_chunk };

	prepare(&fixture);
	queue->playback_configured = true;
	spa_assert_se(pwao_queue_ring_try_push(&queue->pending, 3) == 1);
	offer_playback(&fixture.output[0]);
	playback_process(queue);
	check_failure(queue, "playback.pending-invalid-slot", -EINVAL);
	spa_assert_se(playback.queued == NULL && playback.returned == &fixture.output[0]);

	prepare(&fixture);
	queue->playback_configured = true;
	queue->storage = STORAGE_LEASE;
	spa_assert_se(pwao_queue_ring_try_push(&queue->pending, 3) == 1);
	playback_process(queue);
	check_failure(queue, "playback.pending-invalid-slot", -EINVAL);

	enum publish_fault { STALE_TOKEN, INVALID_LAYOUT, COMPLETION_FULL, QUEUE_ERROR };
	static const struct { enum publish_fault fault; const char *operation; int result; } cases[] = {
		{ STALE_TOKEN, "playback.acquire-pending", -EPROTO },
		{ INVALID_LAYOUT, "playback.transfer", -EINVAL },
		{ COMPLETION_FULL, "playback.complete-copy", -ENOSPC },
		{ QUEUE_ERROR, "playback.publish", -EIO },
	};

	for (size_t i = 0; i < SPA_N_ELEMENTS(cases); i++) {
		enum publish_fault fault = cases[i].fault;

		prepare(&fixture);
		out = 0;
		queue->playback_configured = true;
		offer_capture(&fixture.input[0], NULL);
		capture_process(queue);
		if (fault != INVALID_LAYOUT) {
			fixture.input_buffer[0].n_datas = 1;
			fixture.input_buffer[0].datas = &input;
			fixture.output_buffer[0].n_datas = 1;
			fixture.output_buffer[0].datas = &output;
		}
		switch (fault) {
		case STALE_TOKEN:
			queue->inputs[0].token += UINT64_C(1) << SLOT_TOKEN_BITS;
			break;
		case INVALID_LAYOUT:
			break;
		case COMPLETION_FULL:
			spa_assert_se(pwao_queue_ring_try_push(&queue->completions, 1) == 1);
			break;
		case QUEUE_ERROR:
			playback.queue_result = -EIO;
			break;
		}
		offer_playback(&fixture.output[0]);
		playback_process(queue);
		check_failure(queue, cases[i].operation, cases[i].result);
		if (fault == QUEUE_ERROR) {
			spa_assert_se(playback.queued == &fixture.output[0] && playback.returned == NULL);
			spa_assert_se(queue->outputs[0].output_in_flight && queue->active_outputs == 1);
		} else {
			spa_assert_se(playback.queued == NULL && playback.returned == &fixture.output[0]);
		}
		spa_assert_se(out == (fault == COMPLETION_FULL || fault == QUEUE_ERROR ? in : 0));
	}
}

static void test_pause_and_withdrawal(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;

	prepare(&fixture);
	queue->inputs[0].state = SLOT_ACTIVE;
	pause_ownership_quiescent(queue);
	check_failure(queue, "pause.copy-active", -EPROTO);

	prepare(&fixture);
	offer_capture(&fixture.input[0], NULL);
	capture_process(queue);
	capture.queue_result = -EIO;
	pause_ownership_quiescent(queue);
	check_failure(queue, "pause.return-input", -EIO);
	spa_assert_se(capture.queued == &fixture.input[0] && queue->inputs[0].returns == 0);

	prepare(&fixture);
	queue->storage = STORAGE_LEASE;
	queue->inputs[0].state = SLOT_ACTIVE;
	queue->inputs[0].token = 0;
	queue->outputs[0].output_in_flight = true;
	queue->outputs[0].delivered_input = 0;
	queue->active_outputs = 1;
	pause_ownership_quiescent(queue);
	spa_assert_se(queue->inputs[0].state == SLOT_ACTIVE && capture.queued == NULL);
	playback_remove_buffer(queue, &fixture.output[0]);
	spa_assert_se(queue->n_playback_present == 1 && fixture.output[0].user_data == NULL);
	spa_assert_se(queue->active_outputs == 0 && queue->inputs[0].state == SLOT_COMPLETING);
	drain_completions(queue);
	spa_assert_se(queue->fatal_error == FAILURE_NONE);
	spa_assert_se(queue->inputs[0].state == SLOT_FREE && queue->inputs[0].returns == 1);
	spa_assert_se(capture.queued == &fixture.input[0]);
}

static void test_diagnostics(void)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;

	prepare(&fixture);
	offer_capture(&fixture.input[0], &fixture.input[1]);
	capture_process(queue);
	request_backpressure_recovery(queue);
	check_failure(queue, "backpressure.schedule", -EIO);
	/* A later fault is counted but must not overwrite the first snapshot. */
	MARK_PROTOCOL_ERROR(queue, "second fault", 0, SLOT_PENDING, SLOT_FREE, -EINVAL);
	spa_assert_se(queue->output_stats.protocol_errors == 2);
	check_failure(queue, "backpressure.schedule", -EIO);

	module_properties = pw_properties_new(NULL, NULL);
	spa_assert_se(module_properties != NULL);
	queue->playback_generation_state = PLAYBACK_GENERATION_WAITING;
	queue->ownership_requested = OWNERSHIP_ACTION_PAUSE;
	update_stats(queue, 1);
	spa_assert_se(spa_streq(pw_properties_get(module_properties, "queue.error.operation"), "backpressure.schedule"));
	spa_assert_se(spa_streq(pw_properties_get(module_properties, "queue.error.expected-state"), "blocked"));
	spa_assert_se(spa_streq(pw_properties_get(module_properties, "queue.error.blocked-slot"), "1"));
	spa_assert_se(spa_streq(pw_properties_get(module_properties, "queue.state.output-generation"), "waiting"));
	spa_assert_se(spa_streq(pw_properties_get(module_properties, "queue.state.ownership-transition"), "pause"));
	queue->ownership_requested = OWNERSHIP_ACTION_WITHDRAW;
	update_stats(queue, 1);
	spa_assert_se(spa_streq(pw_properties_get(module_properties, "queue.state.ownership-transition"), "withdraw"));
	spa_assert_se(capture.errors == 1 && playback.errors == 1);
	pw_properties_free(module_properties);
}

static void test_capture_withdrawal(bool disconnect_playback)
{
	struct fixture fixture;
	struct impl *queue = &fixture.queue;

	prepare(&fixture);
	if (!disconnect_playback)
		queue->n_playback_present = queue->n_playback_buffers = 0;
	offer_capture(&fixture.input[0], NULL);
	capture_process(queue);
	spa_assert_se(queue->inputs[0].state == SLOT_PENDING);
	if (disconnect_playback) {
		playback_remove_buffer(queue, &fixture.output[0]);
		playback_remove_buffer(queue, &fixture.output[1]);
	}

	/* A paused capture may already have scheduled an ownership transition. */
	queue->ownership_in_progress = true;
	capture_remove_buffer(queue, &fixture.input[0]);
	spa_assert_se(capture.queued == &fixture.input[0]);
	spa_assert_se(queue->inputs[0].returns == 1);
	spa_assert_se(queue->inputs[0].state == SLOT_FREE);
	spa_assert_se(pwao_queue_ring_size(&queue->pending) == 0);
	spa_assert_se(queue->ownership_requested == OWNERSHIP_ACTION_WITHDRAW);
	capture_remove_buffer(queue, &fixture.input[1]);
	/* pw_stream clears the negotiated pool before announcing format removal. */
	capture_param_changed(queue, SPA_PARAM_Format, NULL);
	spa_assert_se(queue->inputs[0].returns == 1);
	spa_assert_se(queue->n_capture_present == 0);
	spa_assert_se(queue->fatal_error == FAILURE_NONE);
}

int main(int argc, char *argv[])
{
	if (argc == 2 && spa_streq(argv[1], "capture-withdrawal")) {
		struct pw_main_loop *main_loop;

		pw_init(&argc, &argv);
		main_loop = pw_main_loop_new(NULL);
		spa_assert_se(main_loop != NULL);
		capture_loop = pw_main_loop_get_loop(main_loop);
		test_capture_withdrawal(false);
		test_capture_withdrawal(true);
		pw_main_loop_destroy(main_loop);
		pw_deinit();
		return 0;
	}
	test_blocked_retry();
	test_capture_faults();
	test_completion_faults();
	test_playback_faults();
	test_playback_publish_failures();
	test_pause_and_withdrawal();
	test_diagnostics();
	return 0;
}
