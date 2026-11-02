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
	PWAO_IMAGE_FRAME_REQUIRE_PROPERTIES = (1u << 1),
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
	const struct spa_meta *properties;
	const struct spa_meta_property_requirement *required_properties;
	uint32_t n_required_properties;
};

static inline int pwao_image_frame_write(struct spa_buffer *buffer,
		const struct pwao_image_frame *frame, uint32_t requirements)
{
	struct spa_meta *properties;
	struct spa_meta_header *header;
	struct spa_data *data;
	uint64_t end;
	uint32_t i, properties_size = 0;
	const uint32_t valid_header_flags = SPA_META_HEADER_FLAG_DISCONT |
		SPA_META_HEADER_FLAG_CORRUPTED | SPA_META_HEADER_FLAG_MARKER |
		SPA_META_HEADER_FLAG_HEADER | SPA_META_HEADER_FLAG_GAP |
		SPA_META_HEADER_FLAG_DELTA_UNIT;
	const uint32_t valid_chunk_flags = SPA_CHUNK_FLAG_CORRUPTED |
		SPA_CHUNK_FLAG_EMPTY;
	const uint32_t valid_requirements = PWAO_IMAGE_FRAME_REQUIRE_HEADER |
		PWAO_IMAGE_FRAME_REQUIRE_PROPERTIES;

	if (buffer == NULL || frame == NULL ||
			(requirements & ~valid_requirements) != 0 ||
			(frame->header_flags & ~valid_header_flags) != 0 ||
			(frame->chunk_flags & ~valid_chunk_flags) != 0 ||
			(frame->pts < 0 && frame->pts != SPA_TIME_INVALID) ||
			frame->data_index >= buffer->n_datas)
		return -EINVAL;
	data = &buffer->datas[frame->data_index];
	end = (uint64_t) frame->offset + frame->size;
	if (data->chunk == NULL || end > data->maxsize)
		return -ENOSPC;
	for (i = 0; i < buffer->n_metas; i++) {
		uint32_t type = buffer->metas[i].type;
		if (type == 11 || type == 12 || (type >> 16) == 11 || (type >> 16) == 12)
			return -ENOTSUP;
	}
	if (frame->properties != NULL) {
		const struct spa_pod *pod;

		if (!spa_meta_properties_is_valid(frame->properties,
				frame->required_properties, frame->n_required_properties))
			return -EINVAL;
		pod = frame->properties->data;
		properties_size = sizeof(*pod) + pod->size;
	} else if (frame->n_required_properties != 0) {
		return -EINVAL;
	}

	header = (struct spa_meta_header *) spa_buffer_find_meta_data(buffer,
			SPA_META_Header, sizeof(*header));
	if (header == NULL &&
			(requirements & PWAO_IMAGE_FRAME_REQUIRE_HEADER) != 0)
		return -ENOTSUP;
	properties = spa_buffer_find_meta(buffer, SPA_META_Properties);
	if (properties == NULL &&
			((requirements & PWAO_IMAGE_FRAME_REQUIRE_PROPERTIES) != 0 ||
			 frame->properties != NULL))
		return -ENOTSUP;
	if (properties != NULL) {
		if (properties->data == NULL || !SPA_IS_ALIGNED(properties->data, 8) ||
				properties->size < SPA_META_PROPERTIES_MIN_SIZE ||
				properties->size > SPA_META_PROPERTIES_MAX_SIZE ||
				(properties->size & 7u) != 0)
			return -EINVAL;
		if (properties_size > properties->size)
			return -ENOSPC;
		if (frame->properties == NULL) {
			if (!spa_meta_properties_init(properties))
				return -EINVAL;
		} else {
			/* Copy the complete Object before clearing its allocation tail;
			 * the source may alias this output allocation. */
			memmove(properties->data, frame->properties->data, properties_size);
			memset(SPA_PTROFF(properties->data, properties_size, void), 0,
					properties->size - properties_size);
		}
	}

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
