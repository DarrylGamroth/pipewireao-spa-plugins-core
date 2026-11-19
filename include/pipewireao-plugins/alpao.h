/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIREAO_PLUGINS_ALPAO_H
#define PIPEWIREAO_PLUGINS_ALPAO_H

#define SPA_NAME_API_ALPAO_SINK "api.alpao.sink"

#define SPA_KEY_API_ALPAO_BACKEND "api.alpao.backend"
#define SPA_KEY_API_ALPAO_SERIAL "api.alpao.serial"
#define SPA_KEY_API_ALPAO_ACTUATOR_COUNT "api.alpao.actuator-count"
#define SPA_KEY_API_ALPAO_DAQ_FREQUENCY "api.alpao.daq-frequency"
#define SPA_KEY_API_ALPAO_PROFILE "api.alpao.profile"
#define SPA_KEY_API_ALPAO_COMMAND_SCALE "api.alpao.command-scale"

#define SPA_ALPAO_SCHEMA_NORMALIZED_ACTUATOR_COMMAND \
	"org.pipewireao.alpao.normalized-actuator-command/1"
#define SPA_ALPAO_SCHEMA_DEMANDED_PDM_COMMAND \
	"pipewireao/dm-command-opd-um-f32/1"

#define SPA_FGN_ALPAO_LABEL_COMMAND_NORMALIZATION \
	"command-normalization-f32-f64"

#endif /* PIPEWIREAO_PLUGINS_ALPAO_H */
