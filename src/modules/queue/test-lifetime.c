/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

/* Exercise private callbacks after their stream/context authority is fenced.
 * Including the module keeps these checks on the actual static callbacks,
 * without exporting test entrypoints in the installed module. */
#include "module-queue.c"

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

int main(void)
{
	struct impl impl = { 0 };
	spa_invoke_func_t callbacks[] = { ownership_playback_stage,
		ownership_capture_stage, ownership_main_stage, recover_backpressure };

	atomic_init(&impl.destroying, true);
	atomic_init(&impl.refs, 1);
	atomic_init(&impl.ownership_requested, OWNERSHIP_ACTION_WITHDRAW);
	atomic_init(&impl.ownership_in_progress, true);
	/* A dispatched stats timer must not touch the now-absent Streams. */
	update_stats(&impl, 1);
	for (size_t i = 0; i < SPA_N_ELEMENTS(callbacks); i++) {
		atomic_store_explicit(&impl.refs, 2, memory_order_relaxed);
		atomic_store_explicit(&impl.ownership_requested,
				OWNERSHIP_ACTION_WITHDRAW, memory_order_relaxed);
		atomic_store_explicit(&impl.ownership_in_progress, true,
				memory_order_relaxed);
		CHECK(callbacks[i](NULL, true, 1, NULL, 0, &impl) == 0);
		CHECK(atomic_load_explicit(&impl.refs, memory_order_relaxed) == 1);
		if (callbacks[i] != recover_backpressure) {
			CHECK(atomic_load_explicit(&impl.ownership_requested,
					memory_order_relaxed) == OWNERSHIP_ACTION_NONE);
			CHECK(!atomic_load_explicit(&impl.ownership_in_progress,
					memory_order_relaxed));
		}
	}
	return 0;
}
