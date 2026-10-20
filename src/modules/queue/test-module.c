/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/impl.h>

#include "queue.h"

#define CHECK(expression) do { \
	if (!(expression)) { \
		fprintf(stderr, "%s:%d: check failed: %s\n", \
				__FILE__, __LINE__, #expression); \
		abort(); \
	} \
} while (0)

struct node_check {
	const char *capture_prefix;
	const char *playback_prefix;
	const char *group_name;
	const char *capture_description;
	const char *playback_description;
	const char *capture_role;
	const char *playback_role;
	char group_id[64];
	char queue_id[64];
	bool video;
	bool capture_found;
	bool playback_found;
};

static void check_property(const struct pw_properties *properties,
		const char *key, const char *expected)
{
	const char *value = pw_properties_get(properties, key);

	if (expected == NULL ? value != NULL :
			value == NULL || strcmp(value, expected) != 0)
		fprintf(stderr, "property %s: expected %s, got %s\n", key,
				expected == NULL ? "(absent)" : expected,
				value == NULL ? "(absent)" : value);
	CHECK(expected == NULL ? value == NULL :
			value != NULL && strcmp(value, expected) == 0);
}

static int inspect_global(void *data, struct pw_global *global)
{
	struct node_check *check = data;
	struct pw_impl_node *node;
	const struct pw_properties *properties;
	const char *name, *group, *link_group, *group_id, *queue_id;
	bool capture;

	if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node))
		return 0;
	node = pw_global_get_object(global);
	CHECK(node != NULL);
	properties = pw_impl_node_get_properties(node);
	name = pw_properties_get(properties, PW_KEY_NODE_NAME);
	if (name == NULL)
		return 0;
	if (strncmp(name, check->capture_prefix,
			strlen(check->capture_prefix)) == 0)
		capture = true;
	else if (strncmp(name, check->playback_prefix,
			strlen(check->playback_prefix)) == 0)
		capture = false;
	else
		return 0;
	group = pw_properties_get(properties, PW_KEY_NODE_GROUP);
	link_group = pw_properties_get(properties, PW_KEY_NODE_LINK_GROUP);
	CHECK(group != NULL && strncmp(group, capture ?
			"queue.capture-" : "queue.playback-", capture ? 14 : 15) == 0);
	CHECK(link_group != NULL && strncmp(link_group, capture ?
			"queue.capture-link-" : "queue.playback-link-",
			capture ? 19 : 20) == 0);
	check_property(properties, PW_KEY_MEDIA_CLASS, check->video ?
			(capture ? "Video/Sink" : "Video/Source") :
			(capture ? "Data/Sink" : "Data/Source"));
	check_property(properties, "pipewireao.graph.group.name", check->group_name);
	check_property(properties, "pipewireao.graph.group.class", "Queue");
	check_property(properties, PW_KEY_NODE_DESCRIPTION, capture ?
			(check->capture_description == NULL ? name : check->capture_description) :
			(check->playback_description == NULL ? name : check->playback_description));
	check_property(properties, PW_KEY_MEDIA_ROLE,
			capture ? check->capture_role : check->playback_role);
	group_id = pw_properties_get(properties, "pipewireao.graph.group.id");
	queue_id = pw_properties_get(properties, PWAO_QUEUE_ID_PROPERTY);
	CHECK(group_id != NULL && group_id[0] != '\0');
	CHECK(queue_id != NULL && queue_id[0] != '\0');
	if (check->group_id[0] == '\0') {
		CHECK(strlen(group_id) < sizeof(check->group_id));
		snprintf(check->group_id, sizeof(check->group_id), "%s", group_id);
		CHECK(strlen(queue_id) < sizeof(check->queue_id));
		snprintf(check->queue_id, sizeof(check->queue_id), "%s", queue_id);
	} else {
		CHECK(strcmp(check->group_id, group_id) == 0);
		CHECK(strcmp(check->queue_id, queue_id) == 0);
	}
	if (capture)
		check->capture_found = true;
	else
		check->playback_found = true;
	return 0;
}

static void wait_for_nodes(struct pw_main_loop *loop,
		struct pw_context *context, struct node_check *check)
{
	for (uint32_t i = 0; i < 64 &&
			!(check->capture_found && check->playback_found); i++) {
		struct pw_loop *pw_loop = pw_main_loop_get_loop(loop);

		pw_loop_enter(pw_loop);
		(void)pw_loop_iterate(pw_loop, 0);
		pw_loop_leave(pw_loop);
		CHECK(pw_context_for_each_global(context, inspect_global, check) == 0);
	}
	CHECK(check->capture_found && check->playback_found);
}

static void check_other_context(struct pw_main_loop *loop,
		const char *other_group_id)
{
	struct pw_context *context;
	struct pw_impl_module *module;
	struct node_check check = {
		.capture_prefix = "input.queue-", .playback_prefix = "output.queue-",
		.group_name = "Queue",
		.capture_role = "Analysis", .playback_role = "Analysis",
	};

	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
	CHECK(context != NULL);
	module = pw_context_load_module(context, "libpipewire-module-queue",
			"queue.max-buffers=1 queue.overflow=drop-oldest "
			"queue.storage=copy remote.name=internal", NULL);
	CHECK(module != NULL);
	wait_for_nodes(loop, context, &check);
	CHECK(strcmp(other_group_id, check.group_id) != 0);
	pw_context_destroy(context);
}

static void check_presentation(struct pw_main_loop *loop,
		struct pw_context *context, const char *other_group_id)
{
	static const struct {
		const char *args;
		struct node_check check;
	} cases[] = {
		{
			"node.name=unused.name node.description=\"Mirror / Queue [A]-123\" "
			"queue.max-buffers=1 queue.overflow=drop-oldest queue.storage=copy "
			"queue.media=video/raw "
			"capture.props={ node.name=exact-input-123-456 "
			"node.description=\"Exact input / [123-456]\" media.role=Production "
			"pipewireao.graph.group.id=wrong-input "
			"pipewireao.graph.group.name=wrong-input "
			"pipewireao.graph.group.class=wrong-input } "
			"playback.props={ node.name=exact-output-789-012 media.role=DSP "
			"pipewireao.graph.group.id=wrong-output "
			"pipewireao.graph.group.name=wrong-output "
			"pipewireao.graph.group.class=wrong-output }",
			{ .capture_prefix = "exact-input-123-456",
			  .playback_prefix = "exact-output-789-012",
			  .group_name = "Mirror / Queue [A]-123",
			  .capture_description = "Exact input / [123-456]",
			  .capture_role = "Production", .playback_role = "DSP", .video = true },
		},
		{
			"node.name=\"Named / Queue-123-456\" queue.max-buffers=2 "
			"queue.overflow=drop-oldest queue.storage=copy",
			{ .capture_prefix = "input.Named / Queue-123-456-",
			  .playback_prefix = "output.Named / Queue-123-456-",
			  .group_name = "Named / Queue-123-456",
			  .capture_role = "Analysis", .playback_role = "Analysis" },
		},
		{
			"node.name=\"  Named / Queue [123]  \" node.description=\" \\t\\n \" "
			"queue.max-buffers=1 "
			"queue.overflow=drop-oldest queue.storage=copy",
			{ .capture_prefix = "input.  Named / Queue [123]  -",
			  .playback_prefix = "output.  Named / Queue [123]  -",
			  .group_name = "  Named / Queue [123]  ",
			  .capture_role = "Analysis", .playback_role = "Analysis" },
		},
		{
			"node.name=\"\" node.description=\"\" queue.max-buffers=1 "
			"queue.overflow=drop-oldest queue.storage=copy "
			"capture.props={ node.name=empty-title-input node.description=\"\" } "
			"playback.props={ node.name=empty-title-output }",
			{ .capture_prefix = "empty-title-input", .playback_prefix = "empty-title-output",
			  .group_name = "Queue", .capture_description = "",
			  .capture_role = "Analysis", .playback_role = "Analysis" },
		},
		{
			"node.name=\" \\t \" node.description=\" \\n \" queue.max-buffers=1 "
			"queue.overflow=drop-oldest queue.storage=copy "
			"capture.props={ node.name=blank-title-input } "
			"playback.props={ node.name=blank-title-output }",
			{ .capture_prefix = "blank-title-input", .playback_prefix = "blank-title-output",
			  .group_name = "Queue",
			  .capture_role = "Analysis", .playback_role = "Analysis" },
		},
		{
			"node.name=newest queue.max-buffers=1 "
			"queue.overflow=drop-newest queue.storage=copy",
			{ .capture_prefix = "input.newest-", .playback_prefix = "output.newest-",
			  .group_name = "newest" },
		},
		{
			"node.name=pressure queue.max-buffers=1 "
			"queue.overflow=backpressure queue.storage=copy",
			{ .capture_prefix = "input.pressure-", .playback_prefix = "output.pressure-",
			  .group_name = "pressure" },
		},
		{
			"node.name=leased queue.max-buffers=1 "
			"queue.overflow=drop-oldest queue.storage=lease",
			{ .capture_prefix = "input.leased-", .playback_prefix = "output.leased-",
			  .group_name = "leased" },
		},
	};
	struct pw_impl_module *modules[SPA_N_ELEMENTS(cases)];
	struct node_check checks[SPA_N_ELEMENTS(cases)];
	char args[2048];

	for (uint32_t i = 0; i < SPA_N_ELEMENTS(cases); i++) {
		int length = snprintf(args, sizeof(args), "%s remote.name=internal",
				cases[i].args);

		CHECK(length > 0 && (size_t)length < sizeof(args));
		checks[i] = cases[i].check;
		modules[i] = pw_context_load_module(context, "libpipewire-module-queue",
				args, NULL);
		CHECK(modules[i] != NULL);
		wait_for_nodes(loop, context, &checks[i]);
		CHECK(strcmp(other_group_id, checks[i].group_id) != 0);
		for (uint32_t j = 0; j < i; j++)
			CHECK(strcmp(checks[j].group_id, checks[i].group_id) != 0);
	}
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(cases); i++)
		pw_impl_module_destroy(modules[i]);
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
	struct node_check node_check = {
		.capture_prefix = "input.queue-", .playback_prefix = "output.queue-",
		.group_name = "Queue",
		.capture_role = "Analysis", .playback_role = "Analysis",
	};
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
	wait_for_nodes(loop, context, &node_check);
	check_other_context(loop, node_check.group_id);
	check_presentation(loop, context, node_check.group_id);
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
