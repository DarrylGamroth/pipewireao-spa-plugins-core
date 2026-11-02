/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include "buffer-transfer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

int pwao_queue_buffer_validate_layout(const struct spa_buffer *input,
		const struct spa_buffer *output, bool require_output_capacity)
{
	uint32_t i;

	if (input == NULL || output == NULL || input->n_datas == 0 ||
			input->n_datas > PWAO_QUEUE_MAX_DATA_BLOCKS ||
			input->n_datas != output->n_datas ||
			input->n_metas > PWAO_QUEUE_MAX_METAS ||
			!spa_meta_records_validate(input->metas, input->n_metas, NULL, 0) ||
			!spa_meta_records_layout_validate(output->metas, output->n_metas) ||
			output->n_metas > PWAO_QUEUE_MAX_METAS)
		return -EINVAL;
	for (i = 0; i < input->n_metas; i++) {
		const struct spa_meta *in = &input->metas[i];
		const struct spa_meta *out;

		/* Busy is local to each endpoint. Feature metadata advertises a
		 * capability and carries no per-buffer value to transfer. */
		if (spa_meta_type_is_transport_owned(in->type))
			continue;
		out = spa_buffer_find_meta(output, in->type);
		/* Preserve requires storage for every accepted source record. */
		if (out == NULL)
			return -ENOSPC;
		if (out->size != in->size ||
				(in->size > 0 && (in->data == NULL || out->data == NULL)))
			return -EINVAL;
	}
	if (require_output_capacity)
		for (i = 0; i < input->n_datas; i++)
			if (output->datas[i].maxsize < input->datas[i].maxsize)
				return -ENOSPC;
	return 0;
}

int pwao_queue_buffer_transfer(const struct spa_buffer *input,
		struct spa_buffer *output, bool copy_payload)
{
	uint32_t i;
	int result;

	result = pwao_queue_buffer_validate_layout(input, output, copy_payload);
	if (result < 0)
		return result;
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		struct spa_data *out = &output->datas[i];

		if (in->chunk == NULL || out->chunk == NULL ||
				in->chunk->offset > in->maxsize ||
				in->chunk->size > in->maxsize - in->chunk->offset)
			return -EINVAL;
		if (copy_payload) {
			if (in->data == NULL || out->data == NULL ||
					in->chunk->offset > out->maxsize ||
					in->chunk->size >
						out->maxsize - in->chunk->offset)
				return -EINVAL;
		}
	}
	/* Stack descriptors preserve local Busy/feature entries and avoid metadata
	 * heap allocation. All record and payload checks precede any writes. */
	struct spa_meta source[PWAO_QUEUE_MAX_METAS], destination[PWAO_QUEUE_MAX_METAS];
	uint32_t n_source = 0, n_destination = 0;
	for (i = 0; i < input->n_metas; i++)
		if (!spa_meta_type_is_transport_owned(input->metas[i].type))
			source[n_source++] = input->metas[i];
	for (i = 0; i < output->n_metas; i++)
		if (!spa_meta_type_is_transport_owned(output->metas[i].type))
			destination[n_destination++] = output->metas[i];
	if (!spa_meta_records_copy(destination, n_destination, source, n_source))
		return -EINVAL;
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		struct spa_data *out = &output->datas[i];
		if (copy_payload)
			memcpy(SPA_PTROFF(out->data, in->chunk->offset, void),
				SPA_PTROFF(in->data, in->chunk->offset, void), in->chunk->size);
		*out->chunk = *in->chunk;
	}
	return 0;
}

void pwao_queue_buffer_close_fds(int *owned_fds, uint32_t n_owned_fds)
{
	uint32_t i;

	if (owned_fds == NULL)
		return;
	for (i = 0; i < n_owned_fds; i++) {
		if (owned_fds[i] >= 0)
			(void)close(owned_fds[i]);
		owned_fds[i] = -1;
	}
}

int pwao_queue_buffer_alias(const struct spa_buffer *input,
		struct spa_buffer *output, int *owned_fds, uint32_t n_owned_fds)
{
	uint32_t i;
	int result;

	if (owned_fds == NULL || input == NULL ||
			input->n_datas > n_owned_fds)
		return -EINVAL;
	result = pwao_queue_buffer_validate_layout(input, output, false);
	if (result < 0)
		return result;
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		const struct spa_data *out = &output->datas[i];

		if (in->type >= 32 || in->fd < 0 ||
				(in->type != SPA_DATA_MemFd &&
				 in->type != SPA_DATA_DmaBuf) ||
				(out->type != in->type &&
				 (out->type & (1u << in->type)) == 0)) {
			result = -ENOTSUP;
			goto error;
		}
		owned_fds[i] = fcntl(in->fd, F_DUPFD_CLOEXEC, 0);
		if (owned_fds[i] < 0) {
			result = -errno;
			goto error;
		}
	}
	for (i = 0; i < input->n_datas; i++) {
		const struct spa_data *in = &input->datas[i];
		struct spa_data *out = &output->datas[i];

		out->type = in->type;
		out->flags = in->flags;
		out->fd = owned_fds[i];
		out->mapoffset = in->mapoffset;
		out->maxsize = in->maxsize;
		/* The duplicated descriptor owns the lease.  The input mapping belongs
		 * to the capture pool and can disappear independently when that pool is
		 * withdrawn.  Leave mapping to the downstream endpoint so no pointer
		 * crosses pool generations. */
		out->data = NULL;
	}
	return 0;

error:
	pwao_queue_buffer_close_fds(owned_fds, n_owned_fds);
	return result;
}
