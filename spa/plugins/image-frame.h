/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_IMAGE_FRAME_H
#define PIPEWIREAO_IMAGE_FRAME_H

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/utils/defs.h>

enum pwao_image_frame_requirement {
	PWAO_IMAGE_FRAME_REQUIRE_HEADER = (1u << 0),
};

struct pwao_image_frame {
	uint32_t data_index;
	uint32_t header_flags;
	uint32_t chunk_flags;
	uint32_t offset;
	uint32_t size;
	int32_t stride;
	uint32_t header_offset;
	uint64_t sequence;
	int64_t pts;
	const struct spa_meta *metas;
	uint32_t n_metas;
	const struct spa_meta_requirement *required_records;
	uint32_t n_required_records;
};

static inline int pwao_image_frame_write(struct spa_buffer *buffer,
		const struct pwao_image_frame *frame, uint32_t requirements)
{
	struct spa_meta_header *header;
	struct spa_data *data;
	uint64_t end;
	const uint32_t valid_header_flags = SPA_META_HEADER_FLAG_DISCONT |
		SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_MARKER |
		SPA_META_HEADER_FLAG_HEADER | SPA_META_HEADER_FLAG_GAP |
		SPA_META_HEADER_FLAG_DELTA_UNIT;
	const uint32_t valid_chunk_flags = SPA_CHUNK_FLAG_CORRUPTED |
		SPA_CHUNK_FLAG_EMPTY;
	const uint32_t valid_requirements = PWAO_IMAGE_FRAME_REQUIRE_HEADER;

	if (buffer == NULL || frame == NULL ||
			(requirements & ~valid_requirements) != 0 ||
			(frame->header_flags & ~valid_header_flags) != 0 ||
			(frame->chunk_flags & ~valid_chunk_flags) != 0 ||
			(frame->pts < 0 && frame->pts != SPA_TIME_INVALID) ||
			buffer->datas == NULL || frame->data_index >= buffer->n_datas ||
			!spa_meta_records_layout_validate(buffer->metas, buffer->n_metas))
		return -EINVAL;
	data = &buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + frame->size;
	if (data->chunk == NULL || end > data->maxsize)
		return -ENOSPC;
	if (!spa_meta_records_validate(frame->metas, frame->n_metas,
			frame->required_records, frame->n_required_records))
		return -EINVAL;

	header = (struct spa_meta_header *) spa_buffer_find_meta_data(buffer,
			SPA_META_Header, sizeof(*header));
	if (header == NULL)
		return -ENOTSUP;
	/* Copy preflights every record before touching the owned output. Missing
	 * slots and stale facts cannot silently disappear during publication. */
	if (!spa_meta_records_copy(buffer->metas, buffer->n_metas,
			frame->metas, frame->n_metas))
		return -ENOSPC;

	data->chunk->offset = frame->offset;
	data->chunk->size = frame->size;
	data->chunk->stride = frame->stride;
	data->chunk->flags = frame->chunk_flags;

	if (header != NULL) {
		header->flags = frame->header_flags;
		header->offset = frame->header_offset;
		header->seq = frame->sequence;
		header->pts = frame->pts;
		header->dts_offset = 0;
	}
	return 0;
}

#endif /* PIPEWIREAO_IMAGE_FRAME_H */
