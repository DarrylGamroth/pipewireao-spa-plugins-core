/* SPDX-License-Identifier: MIT */
#ifndef PIPEWIREAO_PLUGINS_FITS_H
#define PIPEWIREAO_PLUGINS_FITS_H

#include <spa/param/props.h>

#define SPA_NAME_API_FITS_SOURCE "api.fits.source"

#define SPA_KEY_API_FITS_PATH "api.fits.path"
#define SPA_KEY_API_FITS_HDU "api.fits.hdu"
#define SPA_KEY_API_FITS_SAMPLE_RANK "api.fits.sample-rank"
/** Ndarray axis order: row-major or column-major; image frames default to column-major. */
#define SPA_KEY_API_FITS_LAYOUT "api.fits.layout"
#define SPA_KEY_API_FITS_RATE "api.fits.rate"
#define SPA_KEY_API_FITS_SCHEMA "api.fits.schema"
#define SPA_KEY_API_FITS_PROFILE "api.fits.profile"
#define SPA_KEY_API_FITS_IO_MODE "api.fits.io-mode"
#define SPA_KEY_API_FITS_PREFAULT "api.fits.prefault"
#define SPA_KEY_API_FITS_LOOP "api.fits.loop"
#define SPA_KEY_API_FITS_READINESS "api.fits.readiness"
#define SPA_KEY_API_FITS_OUTPUT_MODE "api.fits.output-mode"
#define SPA_KEY_API_FITS_ROW_BLOCK_ROWS "api.fits.row-block-rows"
#define SPA_KEY_API_FITS_SIMULATED_READOUT_TIME_NS \
	"api.fits.simulated-readout-time-ns"

/** Read-only finite-source state exposed through SPA_PARAM_Props. */
enum spa_fits_source_prop {
	SPA_PROP_FITS_SOURCE_START = SPA_PROP_START_CUSTOM,
	SPA_PROP_FITS_SOURCE_COMPLETED = SPA_PROP_FITS_SOURCE_START,
};

#define SPA_PROP_INFO_FITS_SOURCE_COMPLETED "fits.completed"

#endif /* PIPEWIREAO_PLUGINS_FITS_H */
