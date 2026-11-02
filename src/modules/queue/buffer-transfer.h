/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_QUEUE_BUFFER_TRANSFER_H
#define PIPEWIREAO_QUEUE_BUFFER_TRANSFER_H

#include <stdbool.h>
#include <stdint.h>

#include <spa/buffer/buffer.h>

#define PWAO_QUEUE_MAX_DATA_BLOCKS 8u
#define PWAO_QUEUE_MAX_METAS 64u

int pwao_queue_buffer_validate_layout(const struct spa_buffer *input,
		const struct spa_buffer *output, bool require_output_capacity);

/* Metadata and chunk descriptors are always copied; payload bytes are optional. */
int pwao_queue_buffer_transfer(const struct spa_buffer *input,
		struct spa_buffer *output, bool copy_payload);

/*
 * Configure output data blocks to share the input's MemFd or DmaBuf storage.
 * The caller must initialize every owned_fds entry to -1 and must close any
 * successfully duplicated descriptors with pwao_queue_buffer_close_fds().
 */
int pwao_queue_buffer_alias(const struct spa_buffer *input,
		struct spa_buffer *output, int *owned_fds, uint32_t n_owned_fds);
void pwao_queue_buffer_close_fds(int *owned_fds, uint32_t n_owned_fds);

#endif
