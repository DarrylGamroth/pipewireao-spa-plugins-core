/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/impl.h>

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct node_check {
	bool capture_found;
};

static int inspect_global(void *data, struct pw_global *global)
{
	struct node_check *check = data;
	struct pw_impl_node *node;
	const struct pw_properties *properties;
	const char *name, *group, *link_group;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		return 0;
	node = pw_global_get_object(global);
	CHECK(node != NULL);
	properties = pw_impl_node_get_properties(node);
	name = pw_properties_get(properties, PW_KEY_NODE_NAME);
	if (name == NULL || strncmp(name, "input.queue-", 12) != 0)
		return 0;
	group = pw_properties_get(properties, PW_KEY_NODE_GROUP);
	link_group = pw_properties_get(properties, PW_KEY_NODE_LINK_GROUP);
	CHECK(strcmp(pw_properties_get(properties, PW_KEY_MEDIA_CLASS),
			"Data/Sink") == 0);
	CHECK(group != NULL && strncmp(group, "queue.capture-", 14) == 0);
	CHECK(link_group != NULL &&
			strncmp(link_group, "queue.capture-link-", 19) == 0);
	check->capture_found = true;
	return 0;
}

int main(int argc, char **argv)
{
	char module_dir[PATH_MAX];
	char module_search_path[PATH_MAX * 2u];
	const char *separator;
	const char *base_module_dir;
	void *library;
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct pw_impl_module *module;
	const struct pw_properties *properties;
	struct node_check node_check = { 0 };
	static const char *invalid_args[] = {
		NULL,
		"queue.max-buffers=0 queue.overflow=drop-oldest queue.storage=copy",
		"queue.max-buffers=63 queue.overflow=drop-oldest queue.storage=copy",
		"queue.max-buffers=1 queue.storage=copy",
		"queue.max-buffers=1 queue.overflow=leaky queue.storage=copy",
		"queue.max-buffers=1 queue.overflow=drop-oldest",
		"queue.max-buffers=1 queue.overflow=drop-oldest queue.storage=zero-copy",
		"queue.max-buffers=1 queue.overflow=drop-oldest queue.storage=copy "
			"queue.media=video/encoded",
		"queue.max-buffers=1 queue.overflow=drop-oldest queue.storage=copy "
			"capture.props=\"{\"",
		"queue.max-buffers=1 queue.overflow=drop-oldest queue.storage=copy "
			"capture.props={ node.name=unterminated",
	};
	uint32_t i;
	int path_length;

	CHECK(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	if (library == NULL)
		fprintf(stderr, "%s\n", dlerror());
	CHECK(library != NULL);
	CHECK(dlsym(library, "pipewire__module_init") != NULL);
	CHECK(dlclose(library) == 0);

	separator = strrchr(argv[1], '/');
	CHECK(separator != NULL);
	CHECK((size_t)(separator - argv[1]) < sizeof(module_dir));
	memcpy(module_dir, argv[1], (size_t)(separator - argv[1]));
	module_dir[separator - argv[1]] = '\0';
	base_module_dir = getenv("PIPEWIREAO_MODULE_DIR");
	CHECK(base_module_dir != NULL);
	path_length = snprintf(module_search_path, sizeof(module_search_path),
			"%s:%s", module_dir, base_module_dir);
	CHECK(path_length > 0 && (size_t)path_length < sizeof(module_search_path));
	CHECK(setenv("PIPEWIREAO_MODULE_DIR", module_search_path, 1) == 0);

	pw_init(&argc, &argv);
	loop = pw_main_loop_new(NULL);
	CHECK(loop != NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	CHECK(context != NULL);
	for (i = 0; i < SPA_N_ELEMENTS(invalid_args); i++)
		CHECK(pw_context_load_module(context,
				"libpipewire-module-queue", invalid_args[i], NULL) == NULL);

	module = pw_context_load_module(context, "libpipewire-module-queue",
			"queue.max-buffers=1 "
			"queue.overflow=drop-oldest "
			"queue.storage=copy "
			"queue.media=application/ndarray "
			"remote.name=internal", NULL);
	CHECK(module != NULL);
	properties = pw_impl_module_get_properties(module);
	CHECK(properties != NULL);
	CHECK(strcmp(pw_properties_get(properties, "queue.max-buffers"), "1") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.overflow"),
			"drop-oldest") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.storage"), "copy") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.media"),
			"application/ndarray") == 0);
	CHECK(strstr(pw_properties_get(properties, PW_KEY_MODULE_DESCRIPTION),
			"bounded complete-buffer queue") != NULL);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.publications"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.replacements"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.dropped-arrivals"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.backpressure"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.deliveries"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.completions"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.stats.pool-exhaustions"), "0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.stats.protocol-errors"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.state.pending-depth"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.state.completion-depth"), "0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.state.active-outputs"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.state.configured"),
			"false") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.state.input-format"),
			"none") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.state.ownership-transition"), "idle") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.state.output-generation"), "none") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.state.capture-generation"), "0") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.state.requested-generation"), "0") == 0);
	CHECK(strcmp(pw_properties_get(properties,
			"queue.state.installed-generation"), "0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.operation"),
			"none") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.source-line"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.slot"),
			"n/a") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.expected-state"),
			"n/a") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.observed-state"),
			"n/a") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.pending-depth"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.completion-depth"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.blocked-slot"),
			"n/a") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.slot-acquisitions"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.slot-returns"),
			"0") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.expected-token"),
			"n/a") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.observed-token"),
			"n/a") == 0);
	CHECK(strcmp(pw_properties_get(properties, "queue.error.result"),
			"0") == 0);
	for (i = 0; i < 64 && !node_check.capture_found; i++) {
		struct pw_loop *pw_loop = pw_main_loop_get_loop(loop);

		pw_loop_enter(pw_loop);
		(void)pw_loop_iterate(pw_loop, 0);
		pw_loop_leave(pw_loop);
		CHECK(pw_context_for_each_global(context, inspect_global,
				&node_check) == 0);
	}
	CHECK(node_check.capture_found);
	pw_impl_module_destroy(module);
	module = pw_context_load_module(context, "libpipewire-module-queue",
			"queue.max-buffers=62 "
			"queue.overflow=backpressure "
			"queue.storage=lease "
			"remote.name=internal", NULL);
	CHECK(module != NULL);
	/* Context teardown destroys its Core and Streams before its modules. */
	pw_context_destroy(context);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	CHECK(context != NULL);
	module = pw_context_load_module(context, "libpipewire-module-queue",
			"queue.max-buffers=1 "
			"queue.overflow=drop-oldest "
			"queue.storage=copy "
			"remote.name=internal", NULL);
	CHECK(module != NULL);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
	pw_deinit();
	return 0;
}
