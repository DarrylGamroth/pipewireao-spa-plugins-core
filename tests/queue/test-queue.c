/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "queue.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define STRESS_ITEMS 200000u

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

static void test_boundaries(void)
{
	struct pwao_queue_ring ring;

	CHECK(pwao_queue_ring_init(NULL, 1) == -EINVAL);
	CHECK(pwao_queue_ring_init(&ring, 0) == -EINVAL);
	CHECK(pwao_queue_ring_init(&ring,
			PWAO_QUEUE_RING_MAX_SLOTS + 1u) == -EINVAL);
	CHECK(pwao_queue_ring_init(&ring, 1) == 0);
	CHECK(pwao_queue_ring_size(&ring) == 0);
}

static void test_fifo_and_full(void)
{
	struct pwao_queue_ring ring;
	uint64_t value;

	CHECK(pwao_queue_ring_init(&ring, 3) == 0);
	CHECK(pwao_queue_ring_try_push(&ring, 10) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 11) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 12) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 13) == 0);
	CHECK(pwao_queue_ring_size(&ring) == 3);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 10);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 11);
	CHECK(pwao_queue_ring_try_push(&ring, 13) == 1);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 12);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 13);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 0 &&
			value == UINT64_MAX);
}

static void test_conditional_claim(void)
{
	struct pwao_queue_ring ring;
	uint64_t value;

	CHECK(pwao_queue_ring_init(&ring, 2) == 0);
	CHECK(pwao_queue_ring_try_peek(&ring, &value) == 0 &&
			value == UINT64_MAX);
	CHECK(pwao_queue_ring_try_push(&ring, 41) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 42) == 1);
	CHECK(pwao_queue_ring_try_peek(&ring, &value) == 1 && value == 41);
	CHECK(pwao_queue_ring_try_claim(&ring, 42) == -EAGAIN);
	CHECK(pwao_queue_ring_try_peek(&ring, &value) == 1 && value == 41);
	CHECK(pwao_queue_ring_try_claim(&ring, 41) == 1);
	CHECK(pwao_queue_ring_try_peek(&ring, &value) == 1 && value == 42);
	CHECK(pwao_queue_ring_try_claim(&ring, 42) == 1);
	CHECK(pwao_queue_ring_try_claim(&ring, 42) == 0);
}

static void test_drop_oldest(void)
{
	struct pwao_queue_ring ring;
	uint64_t dropped, value;

	CHECK(pwao_queue_ring_init(&ring, 1) == 0);
	CHECK(pwao_queue_ring_drop_oldest_push(&ring, 1, &dropped) == 1);
	CHECK(dropped == UINT64_MAX);
	CHECK(pwao_queue_ring_drop_oldest_push(&ring, 2, &dropped) == 1);
	CHECK(dropped == 1);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 2);

	CHECK(pwao_queue_ring_init(&ring, 3) == 0);
	CHECK(pwao_queue_ring_try_push(&ring, 20) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 21) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 22) == 1);
	CHECK(pwao_queue_ring_drop_oldest_push(&ring, 23, &dropped) == 1);
	CHECK(dropped == 20);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 21);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 22);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 23);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 0);
}

static void test_reset(void)
{
	struct pwao_queue_ring ring;
	uint64_t value;

	CHECK(pwao_queue_ring_init(&ring, 2) == 0);
	CHECK(pwao_queue_ring_try_push(&ring, 1) == 1);
	pwao_queue_ring_reset(&ring);
	CHECK(pwao_queue_ring_size(&ring) == 0);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 0);
	CHECK(pwao_queue_ring_try_push(&ring, 2) == 1);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 2);
}

static void test_wide_entries(void)
{
	struct pwao_queue_ring ring;
	uint64_t dropped, value;
	const uint64_t first = (UINT64_C(1) << 48) | 3u;
	const uint64_t second = (UINT64_C(2) << 48) | 7u;

	CHECK(pwao_queue_ring_init(&ring, 1) == 0);
	CHECK(pwao_queue_ring_drop_oldest_push(&ring, first, &dropped) == 1);
	CHECK(dropped == UINT64_MAX);
	CHECK(pwao_queue_ring_drop_oldest_push(&ring, second, &dropped) == 1);
	CHECK(dropped == first);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == second);
}

static void test_overflow_policies(void)
{
	struct pwao_queue_ring ring;
	uint64_t released, value;

	CHECK(pwao_queue_ring_init(&ring, 1) == 0);
	CHECK(pwao_queue_ring_admit(&ring, 10,
			PWAO_QUEUE_OVERFLOW_BACKPRESSURE, &released) ==
			PWAO_QUEUE_ADMIT_QUEUED);
	CHECK(released == UINT64_MAX);
	CHECK(pwao_queue_ring_admit(&ring, 11,
			PWAO_QUEUE_OVERFLOW_BACKPRESSURE, &released) ==
			PWAO_QUEUE_ADMIT_BACKPRESSURE);
	CHECK(released == UINT64_MAX);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 10);
	CHECK(pwao_queue_ring_admit(&ring, 11,
			PWAO_QUEUE_OVERFLOW_BACKPRESSURE, &released) ==
			PWAO_QUEUE_ADMIT_QUEUED);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 11);

	CHECK(pwao_queue_ring_init(&ring, 2) == 0);
	CHECK(pwao_queue_ring_try_push(&ring, 20) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 21) == 1);
	CHECK(pwao_queue_ring_admit(&ring, 22,
			PWAO_QUEUE_OVERFLOW_DROP_NEWEST, &released) ==
			PWAO_QUEUE_ADMIT_DROPPED);
	CHECK(released == 22);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 20);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 21);

	CHECK(pwao_queue_ring_init(&ring, 2) == 0);
	CHECK(pwao_queue_ring_try_push(&ring, 30) == 1);
	CHECK(pwao_queue_ring_try_push(&ring, 31) == 1);
	CHECK(pwao_queue_ring_admit(&ring, 32,
			PWAO_QUEUE_OVERFLOW_DROP_OLDEST, &released) ==
			PWAO_QUEUE_ADMIT_REPLACED);
	CHECK(released == 30);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 31);
	CHECK(pwao_queue_ring_try_pop(&ring, &value) == 1 && value == 32);
	CHECK(pwao_queue_ring_admit(&ring, 1,
			(enum pwao_queue_overflow)99, &released) == -EINVAL);
}

struct stress_state {
	struct pwao_queue_ring ring;
	uint64_t *payloads;
	_Atomic uint32_t producer_done;
	_Atomic uint64_t dropped;
	_Atomic uint64_t consumed;
	_Atomic uint32_t failure;
};

static uint64_t payload_for(uint32_t sequence)
{
	return UINT64_C(0x9e3779b97f4a7c15) ^
			((uint64_t)sequence * UINT64_C(0xbf58476d1ce4e5b9));
}

static void *producer_main(void *data)
{
	struct stress_state *state = data;
	uint32_t sequence;

	for (sequence = 0; sequence < STRESS_ITEMS; sequence++) {
		uint64_t dropped;

		state->payloads[sequence] = payload_for(sequence);
		if (pwao_queue_ring_drop_oldest_push(&state->ring, sequence,
				&dropped) != 1) {
			atomic_store_explicit(&state->failure, 1,
					memory_order_relaxed);
			break;
		}
		if (dropped != UINT64_MAX)
			atomic_fetch_add_explicit(&state->dropped, 1,
					memory_order_relaxed);
	}
	atomic_store_explicit(&state->producer_done, 1, memory_order_release);
	return NULL;
}

static void *consumer_main(void *data)
{
	struct stress_state *state = data;
	uint32_t previous = 0;
	int have_previous = 0;

	for (;;) {
		uint64_t entry;
		uint32_t sequence;
		int result = pwao_queue_ring_try_peek(&state->ring, &entry);

		if (result < 0) {
			atomic_store_explicit(&state->failure, 1,
					memory_order_relaxed);
			break;
		}
		if (result == 0) {
			if (atomic_load_explicit(&state->producer_done,
					memory_order_acquire) != 0 &&
					pwao_queue_ring_size(&state->ring) == 0)
				break;
			sched_yield();
			continue;
		}
		result = pwao_queue_ring_try_claim(&state->ring, entry);
		if (result == -EAGAIN || result == 0)
			continue;
		if (result < 0) {
			atomic_store_explicit(&state->failure, 1,
					memory_order_relaxed);
			break;
		}
		if (entry > UINT32_MAX) {
			atomic_store_explicit(&state->failure, 1,
					memory_order_relaxed);
			break;
		}
		sequence = (uint32_t)entry;
		if ((have_previous && sequence <= previous) ||
				state->payloads[sequence] != payload_for(sequence)) {
			atomic_store_explicit(&state->failure, 1,
					memory_order_relaxed);
			break;
		}
		previous = sequence;
		have_previous = 1;
		atomic_fetch_add_explicit(&state->consumed, 1,
				memory_order_relaxed);
	}
	return NULL;
}

static void test_concurrent_replacement(uint32_t capacity)
{
	struct stress_state state = { 0 };
	pthread_t producer, consumer;
	uint64_t accounted;

	state.payloads = calloc(STRESS_ITEMS, sizeof(*state.payloads));
	CHECK(state.payloads != NULL);
	CHECK(pwao_queue_ring_init(&state.ring, capacity) == 0);
	CHECK(pthread_create(&producer, NULL, producer_main, &state) == 0);
	CHECK(pthread_create(&consumer, NULL, consumer_main, &state) == 0);
	CHECK(pthread_join(producer, NULL) == 0);
	CHECK(pthread_join(consumer, NULL) == 0);
	CHECK(atomic_load_explicit(&state.failure, memory_order_relaxed) == 0);
	accounted = atomic_load_explicit(&state.dropped, memory_order_relaxed) +
			atomic_load_explicit(&state.consumed, memory_order_relaxed);
	CHECK(accounted == STRESS_ITEMS);
	free(state.payloads);
}

int main(void)
{
	test_boundaries();
	test_fifo_and_full();
	test_conditional_claim();
	test_drop_oldest();
	test_reset();
	test_wide_entries();
	test_overflow_policies();
	test_concurrent_replacement(1);
	test_concurrent_replacement(17);
	return 0;
}
