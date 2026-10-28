/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_QUEUE_ACQUISITION_H
#define PIPEWIREAO_QUEUE_ACQUISITION_H

#include <errno.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>

/* The pool contract is available before its first Acquisition value is
 * initialized. Require one negotiated version rather than advertising a
 * consumer capability that the retained bytes do not satisfy. */
static inline int acquisition_features(const struct spa_buffer *buffer, uint32_t *features)
{
	uint32_t i, found = 0;

	for (i = 0; i < buffer->n_metas; i++) {
		const struct spa_meta *meta = &buffer->metas[i];

		if ((meta->type >> 16) != SPA_META_Acquisition)
			continue;
		if (found != 0 || meta->size != 0 || (meta->type & 0xffffu) == 0)
			return -EINVAL;
		found = meta->type & 0xffffu;
	}
	if (found != SPA_META_FEATURE_ACQUISITION_VERSION_2 &&
	    found != SPA_META_FEATURE_ACQUISITION_VERSION_3)
		return -EINVAL;
	*features = found;
	return 0;
}

#endif
