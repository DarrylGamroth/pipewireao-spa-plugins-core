/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_PLUGINS_NDARRAY_H
#define PIPEWIREAO_PLUGINS_NDARRAY_H

#define SPA_NAME_API_NDARRAY_FRAME_ASSEMBLY "api.ndarray.frame-assembly"
#define SPA_NAME_API_NDARRAY_VIDEO_VIEW "api.ndarray.video-view"

#define SPA_KEY_API_NDARRAY_FRAME_SIZE "api.ndarray.frame-size"
#define SPA_KEY_API_NDARRAY_FRAME_RATE "api.ndarray.frame-rate"
#define SPA_KEY_API_NDARRAY_ROW_BLOCK_ROWS "api.ndarray.row-block-rows"
#define SPA_KEY_API_NDARRAY_ROW_BLOCK_SCHEMA "api.ndarray.row-block-schema"
#define SPA_KEY_API_NDARRAY_FRAME_SCHEMA "api.ndarray.frame-schema"
#define SPA_KEY_API_NDARRAY_ELEMENT_TYPE "api.ndarray.element-type"
#define SPA_KEY_API_NDARRAY_LAYOUT "api.ndarray.layout"
#define SPA_KEY_API_NDARRAY_SCHEMA "api.ndarray.schema"
#define SPA_KEY_API_NDARRAY_VIDEO_FORMAT "api.ndarray.video-format"

#define SPA_NDARRAY_SCHEMA_RAW_PIXEL_ROW_BLOCK \
	"pipewireao/raw-pixel-row-block-u16/1"
#define SPA_NDARRAY_SCHEMA_CALIBRATED_PIXEL_ROW_BLOCK \
	"pipewireao/calibrated-pixel-row-block-f32/1"

#endif /* PIPEWIREAO_PLUGINS_NDARRAY_H */
