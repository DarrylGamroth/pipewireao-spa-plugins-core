/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/node/node.h>
#include <spa/param/video/raw-utils.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/nuvu.h>

struct capture {
	uint32_t expected;
	uint8_t storage[4096];
	struct spa_pod *param;
};

static void on_result(void *data, int seq SPA_UNUSED, int result,
		uint32_t type, const void *value)
{
	struct capture *capture = data;
	const struct spa_result_node_params *params = value;
	uint32_t size;

	spa_assert_se(result >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS ||
	    params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static const struct spa_node_events events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

static struct spa_pod *enum_format(struct spa_node *node,
		struct capture *capture, enum spa_direction direction)
{
	capture->expected = SPA_PARAM_EnumFormat;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, direction, 0,
			SPA_PARAM_EnumFormat, 0, 1, NULL) == 0);
	spa_assert_se(capture->param != NULL);
	return capture->param;
}

static void expect_invalid(const struct spa_handle_factory *factory,
		const struct spa_dict *info)
{
	struct spa_handle *handle = calloc(1, factory->get_size(factory, info));

	spa_assert_se(handle != NULL);
	spa_assert_se(factory->init(factory, handle, info, NULL, 0) == -EINVAL);
	free(handle);
}

int main(int argc, char **argv)
{
	const struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HNU240_FRAME_RATE, "3015/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HNU240_TRANSPORT_PROFILE,
				SPA_HNU240_CL_FULL_PROFILE),
	};
	const struct spa_dict info = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	const struct spa_dict_item bad_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HNU240_FRAME_RATE, "3015/1"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_HNU240_TRANSPORT_PROFILE, "unknown"),
	};
	const struct spa_dict bad_info = SPA_DICT_INIT(bad_items,
			SPA_N_ELEMENTS(bad_items));
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	struct spa_video_info_raw raw = SPA_VIDEO_INFO_RAW_INIT();
	struct capture capture = { .expected = SPA_ID_INVALID };
	struct spa_handle *handle;
	struct spa_node *node = NULL;
	struct spa_hook listener;
	void *library, *symbol;
	uint32_t index = 0;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_HNU240_DECODER));
	spa_assert_se(enumerate(&factory, &index) == 0);

	expect_invalid(factory, &bad_info);
	handle = calloc(1, factory->get_size(factory, &info));
	spa_assert_se(handle != NULL);
	spa_assert_se(factory->init(factory, handle, &info, NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &events, &capture) == 0);
	spa_assert_se(spa_format_video_raw_parse(enum_format(node, &capture,
			SPA_DIRECTION_INPUT), &raw) >= 0);
	spa_assert_se(raw.format == SPA_VIDEO_FORMAT_GRAY8);
	spa_assert_se(raw.size.width == 1408 && raw.size.height == 131);
	spa_assert_se(raw.framerate.num == 3015 && raw.framerate.denom == 1);
	raw = SPA_VIDEO_INFO_RAW_INIT();
	spa_assert_se(spa_format_video_raw_parse(enum_format(node, &capture,
			SPA_DIRECTION_OUTPUT), &raw) >= 0);
	spa_assert_se(raw.format == SPA_VIDEO_FORMAT_GRAY16_LE);
	spa_assert_se(raw.size.width == 240 && raw.size.height == 240);
	spa_assert_se(raw.framerate.num == 3015 && raw.framerate.denom == 1);
	spa_hook_remove(&listener);
	spa_assert_se(handle->clear(handle) == 0);
	free(handle);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
