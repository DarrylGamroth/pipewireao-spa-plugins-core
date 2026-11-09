/* SPDX-FileCopyrightText: Copyright © 2026 PipeWireAO contributors */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <dlfcn.h>

#include <spa/node/command.h>
#include <spa/node/io.h>
#include <spa/node/keys.h>
#include <spa/node/node.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/format-utils.h>
#include <spa/param/ndarray-utils.h>
#include <spa/param/video/raw-utils.h>
#include <spa/pod/compare.h>
#include <spa/pod/iter.h>
#include <spa/support/plugin.h>

#include <pipewireao-plugins/discard.h>

struct param_capture {
	uint32_t expected;
	uint8_t storage[4096];
	struct spa_pod *param;
	const char *node_name;
	const char *node_description;
	const char *minimum_buffer_size;
	const char *port_name;
	uint64_t port_flags;
};

static void on_info(void *data, const struct spa_node_info *info)
{
	struct param_capture *capture = data;

	if (info->props == NULL)
		return;
	capture->node_name = spa_dict_lookup(info->props, SPA_KEY_NODE_NAME);
	capture->node_description = spa_dict_lookup(info->props,
			SPA_KEY_NODE_DESCRIPTION);
	capture->minimum_buffer_size = spa_dict_lookup(info->props,
			SPA_KEY_API_PIPEWIREAO_DISCARD_MINIMUM_BUFFER_SIZE);
}

static void on_port_info(void *data, enum spa_direction direction,
		uint32_t port_id, const struct spa_port_info *info)
{
	struct param_capture *capture = data;

	if (direction != SPA_DIRECTION_INPUT || port_id != 0 || info == NULL)
		return;
	capture->port_flags = info->flags;
	if (info->props != NULL)
		capture->port_name = spa_dict_lookup(info->props,
				SPA_KEY_PORT_NAME);
}

static void on_result(void *data, int seq, int result, uint32_t type,
		const void *value)
{
	struct param_capture *capture = data;
	const struct spa_result_node_params *params;
	uint32_t size;

	(void)seq;
	spa_assert_se(result >= 0);
	if (type != SPA_RESULT_TYPE_NODE_PARAMS)
		return;
	params = value;
	if (params->id != capture->expected || params->param == NULL)
		return;
	size = SPA_POD_SIZE(params->param);
	spa_assert_se(size <= sizeof(capture->storage));
	memcpy(capture->storage, params->param, size);
	capture->param = (struct spa_pod *)capture->storage;
}

static const struct spa_node_events node_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.info = on_info,
	.port_info = on_port_info,
	.result = on_result,
};

static struct spa_pod *enum_port_one(struct spa_node *node,
		struct param_capture *capture, uint32_t id, const struct spa_pod *filter)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_port_enum_params(node, 1, SPA_DIRECTION_INPUT, 0,
			id, 0, 1, filter) == 0);
	return capture->param;
}

static struct spa_pod *enum_node_one(struct spa_node *node,
		struct param_capture *capture, uint32_t id, uint32_t start)
{
	capture->expected = id;
	capture->param = NULL;
	spa_assert_se(spa_node_enum_params(node, 1, id, start, 1, NULL) == 0);
	return capture->param;
}

static struct spa_pod *build_audio_format(uint8_t *storage, size_t size,
		uint32_t object_id, bool mandatory_media_type)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	struct spa_pod_frame object;

	spa_pod_builder_push_object(&builder, &object,
			SPA_TYPE_OBJECT_Format, object_id);
	spa_pod_builder_prop(&builder, SPA_FORMAT_mediaType,
			mandatory_media_type ? SPA_POD_PROP_FLAG_MANDATORY : 0);
	spa_pod_builder_id(&builder, SPA_MEDIA_TYPE_audio);
	spa_pod_builder_add(&builder,
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_F32_LE),
			SPA_FORMAT_AUDIO_rate, SPA_POD_Int(48000),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(2),
			0);
	return spa_pod_builder_pop(&builder, &object);
}

static struct spa_pod *build_video_format(uint8_t *storage, size_t size,
		uint32_t object_id)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const struct spa_rectangle dimensions = SPA_RECTANGLE(13, 7);
	const struct spa_fraction rate = SPA_FRACTION(29, 1);

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, object_id,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_RGB),
			SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&dimensions),
			SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&rate));
}

static struct spa_pod *build_ndarray_format(uint8_t *storage, size_t size,
		uint32_t object_id)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);
	const int32_t shape[] = { 3, 5, 7 };

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, object_id,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_application),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_ndarray),
			SPA_FORMAT_NDARRAY_elementType,
			SPA_POD_Id(SPA_ELEMENT_TYPE_U16_LE),
			SPA_FORMAT_NDARRAY_shape, SPA_POD_Array(sizeof(int32_t),
					SPA_TYPE_Int, SPA_N_ELEMENTS(shape), shape),
			SPA_FORMAT_NDARRAY_layout,
			SPA_POD_Id(SPA_NDARRAY_LAYOUT_COLUMN_MAJOR),
			SPA_FORMAT_NDARRAY_schema,
			SPA_POD_String("org.pipewireao.test.any-ndarray/7"));
}

static struct spa_pod *build_unfixed_format(uint8_t *storage, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType, SPA_POD_CHOICE_ENUM_Id(2,
					SPA_MEDIA_TYPE_audio, SPA_MEDIA_TYPE_video));
}

static struct spa_pod *build_props(uint8_t *storage, size_t size)
{
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage,
			(uint32_t)size);

	return spa_pod_builder_add_object(&builder,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_mute, SPA_POD_Bool(false));
}

struct test_buffer {
	struct spa_buffer buffer;
	struct spa_data datas[2];
	struct spa_chunk chunks[2];
	uint8_t payload[46];
};

static void init_test_buffers(struct test_buffer *empty,
		struct test_buffer *multi)
{
	memset(empty, 0, sizeof(*empty));
	memset(multi, 0, sizeof(*multi));
	multi->datas[0].type = SPA_DATA_DmaBuf;
	multi->datas[0].fd = 123;
	multi->datas[0].maxsize = 12;
	multi->datas[0].data = multi->payload;
	multi->datas[0].chunk = &multi->chunks[0];
	multi->chunks[0].size = 12;
	multi->datas[1].type = SPA_DATA_MemId;
	multi->datas[1].fd = -1;
	multi->datas[1].maxsize = 34;
	multi->datas[1].data = &multi->payload[12];
	multi->datas[1].chunk = &multi->chunks[1];
	multi->chunks[1].size = 34;
	multi->buffer.n_datas = SPA_N_ELEMENTS(multi->datas);
	multi->buffer.datas = multi->datas;
	for (uint32_t i = 0; i < SPA_N_ELEMENTS(multi->payload); i++)
		multi->payload[i] = (uint8_t)i;
}

static uint64_t digest_payload(uint64_t digest, const uint8_t *payload,
		size_t size)
{
	for (size_t i = 0; i < size; i++) {
		digest ^= payload[i];
		digest *= UINT64_C(1099511628211);
	}
	return digest;
}

static void expect_format_filter(struct spa_node *node,
		struct param_capture *capture, struct spa_pod *format,
		uint32_t expected_media_type, uint32_t expected_media_subtype)
{
	struct spa_pod *advertised = enum_port_one(node, capture,
			SPA_PARAM_EnumFormat, format);
	uint32_t media_type = SPA_ID_INVALID;
	uint32_t media_subtype = SPA_ID_INVALID;

	spa_assert_se(advertised != NULL);
	spa_assert_se(spa_pod_compare(advertised, format) == 0);
	spa_assert_se(spa_format_parse(advertised, &media_type,
			&media_subtype) >= 0);
	spa_assert_se(media_type == expected_media_type);
	spa_assert_se(media_subtype == expected_media_subtype);
}

static void expect_metric_info(struct spa_node *node,
		struct param_capture *capture, uint32_t index, uint32_t expected_id,
		const char *expected_name)
{
	struct spa_pod *info = enum_node_one(node, capture,
			SPA_PARAM_PropInfo, index);
	uint32_t id = SPA_ID_INVALID;
	const char *name = NULL;
	bool writable = true;

	spa_assert_se(info != NULL);
	spa_assert_se(spa_pod_parse_object(info,
			SPA_TYPE_OBJECT_PropInfo, NULL,
			SPA_PROP_INFO_id, SPA_POD_Id(&id),
			SPA_PROP_INFO_name, SPA_POD_String(&name),
			SPA_PROP_INFO_params, SPA_POD_Bool(&writable)) >= 0);
	spa_assert_se(id == expected_id);
	spa_assert_se(spa_streq(name, expected_name));
	spa_assert_se(!writable);
}

static void expect_metrics(struct spa_node *node,
		struct param_capture *capture, int64_t expected_buffers,
		int64_t expected_blocks, int64_t expected_bytes,
		int64_t expected_errors, int64_t expected_process_calls,
		uint64_t expected_digest, int64_t expected_digest_bytes)
{
	struct spa_pod *props = enum_node_one(node, capture, SPA_PARAM_Props, 0);
	int64_t buffers = -1;
	int64_t blocks = -1;
	int64_t bytes = -1;
	int64_t errors = -1;
	int64_t process_calls = -1;
	int64_t digest = 0;
	int64_t digest_bytes = -1;

	spa_assert_se(props != NULL);
	spa_assert_se(spa_pod_parse_object(props,
			SPA_TYPE_OBJECT_Props, NULL,
			SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS, SPA_POD_Long(&buffers),
			SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS, SPA_POD_Long(&blocks),
			SPA_PROP_PIPEWIREAO_DISCARD_BYTES, SPA_POD_Long(&bytes),
			SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS,
			SPA_POD_Long(&errors),
			SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS,
			SPA_POD_Long(&process_calls),
			SPA_PROP_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST,
			SPA_POD_Long(&digest),
			SPA_PROP_PIPEWIREAO_DISCARD_DIGEST_BYTES,
			SPA_POD_Long(&digest_bytes)) >= 0);
	spa_assert_se(buffers == expected_buffers);
	spa_assert_se(blocks == expected_blocks);
	spa_assert_se(bytes == expected_bytes);
	spa_assert_se(errors == expected_errors);
	spa_assert_se(process_calls == expected_process_calls);
	spa_assert_se((uint64_t)digest == expected_digest);
	spa_assert_se(digest_bytes == expected_digest_bytes);
}

static void exercise(const struct spa_handle_factory *factory)
{
	const size_t size = spa_handle_factory_get_size(factory, NULL);
	struct spa_handle *handle = calloc(1, size);
	struct spa_node *node = NULL;
	struct spa_hook listener;
	struct param_capture capture = { .expected = SPA_ID_INVALID };
	uint8_t audio_storage[1024];
	uint8_t mandatory_audio_storage[1024];
	uint8_t video_storage[1024];
	uint8_t ndarray_storage[1024];
	uint8_t unfixed_storage[1024];
	uint8_t props_storage[1024];
	struct spa_pod *audio, *mandatory_audio, *video, *ndarray, *unfixed;
	struct spa_pod *props, *node_io;
	const struct spa_pod_prop *buffer_size_property;
	const struct spa_pod *buffer_size_values;
	uint32_t buffer_size_value_count = 0;
	uint32_t buffer_size_choice = SPA_CHOICE_None;
	uint32_t node_io_id = SPA_ID_INVALID;
	int32_t node_io_size = 0;
	int32_t buffer_size = -1;
	struct test_buffer storage[2];
	struct spa_buffer *buffers[2];
	struct spa_io_buffers io = SPA_IO_BUFFERS_INIT;
	struct spa_io_position position = { 0 };
	struct spa_command start = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start);
	struct spa_command pause = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause);
	struct spa_command suspend = SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend);
	struct spa_pod *advertised;
	const struct spa_dict_item node_items[] = {
		SPA_DICT_ITEM_INIT(SPA_KEY_NODE_NAME, "discard-test"),
		SPA_DICT_ITEM_INIT(SPA_KEY_NODE_DESCRIPTION,
				"Format-independent test sink"),
		SPA_DICT_ITEM_INIT(SPA_KEY_API_PIPEWIREAO_DISCARD_MINIMUM_BUFFER_SIZE,
				"64"),
	};
	const struct spa_dict node_info = SPA_DICT_INIT_ARRAY(node_items);
	uint64_t digest = UINT64_C(14695981039346656037);

	spa_assert_se(handle != NULL);
	spa_assert_se(spa_handle_factory_init(factory, handle, &node_info,
			NULL, 0) == 0);
	spa_assert_se(spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Node,
			(void **)&node) == 0);
	spa_assert_se(spa_node_add_listener(node, &listener, &node_events,
			&capture) == 0);
	spa_assert_se(spa_streq(capture.node_name, "discard-test"));
	spa_assert_se(spa_streq(capture.node_description,
			"Format-independent test sink"));
	spa_assert_se(spa_streq(capture.minimum_buffer_size, "64"));
	spa_assert_se(spa_streq(capture.port_name, "in"));
	spa_assert_se(capture.port_flags ==
			(SPA_PORT_FLAG_NO_REF | SPA_PORT_FLAG_TERMINAL));
	node_io = enum_node_one(node, &capture, SPA_PARAM_IO, 0);
	spa_assert_se(node_io != NULL);
	spa_assert_se(spa_pod_parse_object(node_io,
			SPA_TYPE_OBJECT_ParamIO, NULL,
			SPA_PARAM_IO_id, SPA_POD_Id(&node_io_id),
			SPA_PARAM_IO_size, SPA_POD_Int(&node_io_size)) >= 0);
	spa_assert_se(node_io_id == SPA_IO_Position);
	spa_assert_se(node_io_size == (int32_t)sizeof(struct spa_io_position));
	spa_assert_se(enum_node_one(node, &capture, SPA_PARAM_IO, 1) == NULL);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Position, &position,
			sizeof(position) - 1u) == -EINVAL);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Position, &position,
			sizeof(position)) == 0);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Position, NULL, 0) == 0);
	spa_assert_se(spa_node_set_io(node, SPA_IO_Clock, NULL, 0) == -ENOENT);

	advertised = enum_port_one(node, &capture, SPA_PARAM_EnumFormat, NULL);
	spa_assert_se(advertised != NULL);
	spa_assert_se(spa_pod_is_object(advertised));
	spa_assert_se(SPA_POD_OBJECT_TYPE(advertised) == SPA_TYPE_OBJECT_Format);
	spa_assert_se(!spa_pod_object_has_props(
			(const struct spa_pod_object *)advertised));

	audio = build_audio_format(audio_storage, sizeof(audio_storage),
			SPA_PARAM_EnumFormat, false);
	mandatory_audio = build_audio_format(mandatory_audio_storage,
			sizeof(mandatory_audio_storage), SPA_PARAM_EnumFormat, true);
	video = build_video_format(video_storage, sizeof(video_storage),
			SPA_PARAM_EnumFormat);
	ndarray = build_ndarray_format(ndarray_storage, sizeof(ndarray_storage),
			SPA_PARAM_EnumFormat);
	expect_format_filter(node, &capture, audio, SPA_MEDIA_TYPE_audio,
			SPA_MEDIA_SUBTYPE_raw);
	expect_format_filter(node, &capture, mandatory_audio,
			SPA_MEDIA_TYPE_audio, SPA_MEDIA_SUBTYPE_raw);
	expect_format_filter(node, &capture, video, SPA_MEDIA_TYPE_video,
			SPA_MEDIA_SUBTYPE_raw);
	expect_format_filter(node, &capture, ndarray,
			SPA_MEDIA_TYPE_application, SPA_MEDIA_SUBTYPE_ndarray);

	unfixed = build_unfixed_format(unfixed_storage, sizeof(unfixed_storage));
	props = build_props(props_storage, sizeof(props_storage));
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, unfixed) == -EINVAL);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, props) == -EINVAL);
	spa_assert_se(spa_node_send_command(node, &start) == -EIO);

	SPA_POD_OBJECT_ID(video) = SPA_PARAM_Format;
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, SPA_NODE_PARAM_FLAG_TEST_ONLY, video) == 0);
	spa_assert_se(enum_port_one(node, &capture, SPA_PARAM_Format, NULL) == NULL);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, video) == 0);
	advertised = enum_port_one(node, &capture, SPA_PARAM_Format, NULL);
	spa_assert_se(advertised != NULL);
	spa_assert_se(spa_pod_compare(advertised, video) == 0);
	advertised = enum_port_one(node, &capture, SPA_PARAM_Buffers, NULL);
	spa_assert_se(advertised != NULL);
	buffer_size_property = spa_pod_find_prop(advertised, NULL,
			SPA_PARAM_BUFFERS_size);
	spa_assert_se(buffer_size_property != NULL);
	buffer_size_values = spa_pod_get_values(&buffer_size_property->value,
			&buffer_size_value_count, &buffer_size_choice);
	spa_assert_se(buffer_size_values != NULL);
	spa_assert_se(buffer_size_choice == SPA_CHOICE_Range);
	spa_assert_se(buffer_size_value_count == 3);
	spa_assert_se(spa_pod_get_int(buffer_size_values, &buffer_size) == 0);
	spa_assert_se(buffer_size == 64);
	spa_assert_se(spa_node_send_command(node, &start) == -EIO);

	init_test_buffers(&storage[0], &storage[1]);
	buffers[0] = &storage[0].buffer;
	buffers[1] = &storage[1].buffer;
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == -EIO);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);

	io.buffer_id = 0;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(io.status == SPA_STATUS_NEED_DATA);
	io.buffer_id = 1;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(io.status == SPA_STATUS_NEED_DATA);
	digest = digest_payload(digest, storage[1].payload,
			SPA_N_ELEMENTS(storage[1].payload));
	expect_metrics(node, &capture, 2, 2, 46, 0, 4, digest, 46);

	spa_assert_se(spa_node_send_command(node, &pause) == 0);
	io.buffer_id = 1;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	spa_assert_se(io.status == SPA_STATUS_NEED_DATA);
	digest = digest_payload(digest, storage[1].payload,
			SPA_N_ELEMENTS(storage[1].payload));
	expect_metrics(node, &capture, 3, 4, 92, 0, 6, digest, 92);

	io.buffer_id = 99;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == -EPROTO);
	spa_assert_se(io.status == -EPROTO);
	expect_metrics(node, &capture, 3, 4, 92, 1, 7, digest, 92);

	expect_metric_info(node, &capture, 0,
			SPA_PROP_PIPEWIREAO_DISCARD_BUFFERS,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_BUFFERS);
	expect_metric_info(node, &capture, 1,
			SPA_PROP_PIPEWIREAO_DISCARD_DATA_BLOCKS,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_DATA_BLOCKS);
	expect_metric_info(node, &capture, 2,
			SPA_PROP_PIPEWIREAO_DISCARD_BYTES,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_BYTES);
	expect_metric_info(node, &capture, 3,
			SPA_PROP_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_PROTOCOL_ERRORS);
	expect_metric_info(node, &capture, 4,
			SPA_PROP_PIPEWIREAO_DISCARD_PROCESS_CALLS,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_PROCESS_CALLS);
	expect_metric_info(node, &capture, 5,
			SPA_PROP_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_PAYLOAD_DIGEST);
	expect_metric_info(node, &capture, 6,
			SPA_PROP_PIPEWIREAO_DISCARD_DIGEST_BYTES,
			SPA_PROP_INFO_PIPEWIREAO_DISCARD_DIGEST_BYTES);
	spa_assert_se(enum_node_one(node, &capture, SPA_PARAM_PropInfo, 7) == NULL);
	spa_assert_se(spa_node_set_param(node, SPA_PARAM_Props, 0, props) == -EPERM);

	SPA_POD_OBJECT_ID(audio) = SPA_PARAM_Format;
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, audio) == -EBUSY);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == -EBUSY);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, NULL) == -EBUSY);

	/* PipeWire can clear the pool before Pause or I/O detach. */
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			NULL, 0) == 0);
	io.buffer_id = 1;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	spa_assert_se(io.status == SPA_STATUS_HAVE_DATA);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, NULL, 0) == 0);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, NULL) == 0);
	spa_assert_se(spa_node_send_command(node, &start) == -EIO);
	spa_assert_se(spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Format, 0, video) == 0);
	spa_assert_se(spa_node_port_use_buffers(node, SPA_DIRECTION_INPUT, 0, 0,
			buffers, SPA_N_ELEMENTS(buffers)) == 0);
	spa_assert_se(spa_node_port_set_io(node, SPA_DIRECTION_INPUT, 0,
			SPA_IO_Buffers, &io, sizeof(io)) == 0);
	io.buffer_id = 1;
	io.status = SPA_STATUS_HAVE_DATA;
	spa_assert_se(spa_node_process(node) == SPA_STATUS_OK);
	spa_assert_se(spa_node_send_command(node, &start) == 0);
	spa_assert_se(spa_node_process(node) == SPA_STATUS_NEED_DATA);
	digest = digest_payload(digest, storage[1].payload,
			SPA_N_ELEMENTS(storage[1].payload));
	expect_metrics(node, &capture, 4, 6, 138, 1, 10, digest, 138);
	spa_assert_se(spa_node_send_command(node, &suspend) == 0);

	spa_hook_remove(&listener);
	spa_assert_se(spa_handle_clear(handle) == 0);
	free(handle);
}

int main(int argc, char **argv)
{
	void *library, *symbol;
	spa_handle_factory_enum_func_t enumerate;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;

	spa_assert_se(argc == 2);
	library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	spa_assert_se(library != NULL);
	symbol = dlsym(library, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);
	spa_assert_se(symbol != NULL);
	spa_assert_se(sizeof(enumerate) == sizeof(symbol));
	memcpy(&enumerate, &symbol, sizeof(enumerate));
	spa_assert_se(enumerate(&factory, &index) == 1);
	spa_assert_se(factory != NULL);
	spa_assert_se(spa_streq(factory->name,
			SPA_NAME_API_PIPEWIREAO_DISCARD));
	spa_assert_se(enumerate(&factory, &index) == 0);
	exercise(factory);
	spa_assert_se(dlclose(library) == 0);
	return 0;
}
