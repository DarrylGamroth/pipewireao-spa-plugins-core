/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <spa/support/plugin.h>
#include <spa/utils/string.h>

#include <pipewireao-plugins/pyrtc.h>

int main(int argc, char **argv)
{
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	struct spa_handle *handle;
	uint32_t index = 0;
	void *library, *symbol;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	spa_assert_se(sizeof(enumerate) == sizeof(symbol));
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_PYRTC_SOURCE));
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL &&
			spa_streq(factory->name, SPA_NAME_API_PYRTC_SINK));
	spa_assert_se(enumerate(&factory, &index) == 0);

	handle = calloc(1, factory->get_size(factory, NULL));
	spa_assert_se(handle != NULL);
	spa_assert_se(factory->init(factory, handle, NULL, NULL, 0) == -EINVAL);
	free(handle);
	{
		const struct spa_dict_item items[] = {
			SPA_DICT_ITEM_INIT(SPA_KEY_API_PYRTC_NAME, "test"),
			SPA_DICT_ITEM_INIT(SPA_KEY_API_PYRTC_SCHEMA, "test/1"),
			SPA_DICT_ITEM_INIT(SPA_KEY_API_PYRTC_ACCESS, "invalid"),
		};
		const struct spa_dict info = SPA_DICT_INIT(items,
				SPA_N_ELEMENTS(items));

		handle = calloc(1, factory->get_size(factory, &info));
		spa_assert_se(handle != NULL);
		spa_assert_se(factory->init(factory, handle, &info, NULL, 0) ==
				-EINVAL);
		free(handle);
	}
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
