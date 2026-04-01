/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * conference_video_filter.c -- FFmpeg Video Filter for User ID/Name Overlay
 *
 */

#include <mod_conference.h>
// #define HAVE_LIBAVFILTER
#ifdef HAVE_LIBAVFILTER
#include <libavutil/log.h>

#define CONFERENCE_VIDEO_LABEL_BASE_HEIGHT 24
#define CONFERENCE_VIDEO_LABEL_BASE_BOTTOM_MARGIN 5
#define CONFERENCE_VIDEO_LABEL_BASE_X 10
#define CONFERENCE_VIDEO_LABEL_BASE_PADDING_X 6
#define CONFERENCE_VIDEO_LABEL_BASE_ICON_SIZE 18
#define CONFERENCE_VIDEO_LABEL_BASE_ICON_GAP 6
#define CONFERENCE_VIDEO_LABEL_BASE_FONT_SIZE 14
#define CONFERENCE_VIDEO_LABEL_BASE_TEXT_WIDTH 12
#define CONFERENCE_VIDEO_LABEL_BASE_SPACE_WIDTH 7
#define CONFERENCE_VIDEO_LABEL_SCALE_REFERENCE_WIDTH 320
#define CONFERENCE_VIDEO_LABEL_SCALE_REFERENCE_HEIGHT 180
#define CONFERENCE_VIDEO_LABEL_SCALE_MIN_PCT 60
#define CONFERENCE_VIDEO_LABEL_SCALE_MAX_PCT 100
#define CONFERENCE_VIDEO_LABEL_SCALE_ROUNDING 50
#define CONFERENCE_VIDEO_LABEL_MIN_HEIGHT 14
#define CONFERENCE_VIDEO_LABEL_MIN_BOTTOM_MARGIN 3
#define CONFERENCE_VIDEO_LABEL_MIN_X 4
#define CONFERENCE_VIDEO_LABEL_MIN_PADDING_X 3
#define CONFERENCE_VIDEO_LABEL_MIN_ICON_SIZE 8
#define CONFERENCE_VIDEO_LABEL_MIN_ICON_GAP 3
#define CONFERENCE_VIDEO_LABEL_MIN_FONT_SIZE 10
#define CONFERENCE_VIDEO_LABEL_MIN_TEXT_WIDTH 8
#define CONFERENCE_VIDEO_LABEL_MIN_SPACE_WIDTH 4

typedef struct conference_video_label_style_s {
	int label_height;
	int bottom_margin;
	int x;
	int padding_x;
	int icon_size;
	int icon_gap;
	int font_size;
	int text_width;
	int space_width;
} conference_video_label_style_t;

static int conference_video_label_scale_percent(int width, int height)
{
	int width_scale = (width * CONFERENCE_VIDEO_LABEL_SCALE_MAX_PCT) /
		CONFERENCE_VIDEO_LABEL_SCALE_REFERENCE_WIDTH;
	int height_scale = (height * CONFERENCE_VIDEO_LABEL_SCALE_MAX_PCT) /
		CONFERENCE_VIDEO_LABEL_SCALE_REFERENCE_HEIGHT;
	int scale_percent = width_scale < height_scale ? width_scale : height_scale;

	if (scale_percent < CONFERENCE_VIDEO_LABEL_SCALE_MIN_PCT) {
		scale_percent = CONFERENCE_VIDEO_LABEL_SCALE_MIN_PCT;
	}

	if (scale_percent > CONFERENCE_VIDEO_LABEL_SCALE_MAX_PCT) {
		scale_percent = CONFERENCE_VIDEO_LABEL_SCALE_MAX_PCT;
	}

	return scale_percent;
}

static int conference_video_label_scale_value(int base_value, int scale_percent, int min_value)
{
	int scaled_value = (base_value * scale_percent + CONFERENCE_VIDEO_LABEL_SCALE_ROUNDING) /
		CONFERENCE_VIDEO_LABEL_SCALE_MAX_PCT;

	if (scaled_value < min_value) {
		scaled_value = min_value;
	}

	return scaled_value;
}

static conference_video_label_style_t conference_video_label_style(int width, int height)
{
	int scale_percent = conference_video_label_scale_percent(width, height);
	conference_video_label_style_t style;

	style.label_height = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_HEIGHT, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_HEIGHT);
	style.bottom_margin = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_BOTTOM_MARGIN, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_BOTTOM_MARGIN);
	style.x = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_X, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_X);
	style.padding_x = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_PADDING_X, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_PADDING_X);
	style.icon_size = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_ICON_SIZE, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_ICON_SIZE);
	style.icon_gap = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_ICON_GAP, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_ICON_GAP);
	style.font_size = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_FONT_SIZE, scale_percent,
		CONFERENCE_VIDEO_LABEL_MIN_FONT_SIZE);
	style.text_width = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_TEXT_WIDTH, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_TEXT_WIDTH);
	style.space_width = conference_video_label_scale_value(
		CONFERENCE_VIDEO_LABEL_BASE_SPACE_WIDTH, scale_percent, CONFERENCE_VIDEO_LABEL_MIN_SPACE_WIDTH);

	return style;
}

static int conference_video_label_top_from_style(int frame_height,
	const conference_video_label_style_t *style)
{
	return frame_height - (style->bottom_margin + style->label_height);
}

static int conference_video_label_icon_top_from_style(int frame_height,
	const conference_video_label_style_t *style)
{
	int label_top = conference_video_label_top_from_style(frame_height, style);

	return label_top + (style->label_height - style->icon_size) / 2;
}

int conference_video_label_top_for_size(int width, int height)
{
	conference_video_label_style_t style = conference_video_label_style(width, height);

	return conference_video_label_top_from_style(height, &style);
}

int conference_video_label_icon_top_for_size(int width, int height)
{
	conference_video_label_style_t style = conference_video_label_style(width, height);

	return conference_video_label_icon_top_from_style(height, &style);
}

static int conference_video_label_font_size(const conference_video_label_style_t *style)
{
	return style->font_size;
}

const char *conference_video_mic_icon_pixel_format(void)
{
	/* 透明 PNG 需要保留 alpha，不能在图标子链里提前降到 yuv420p。 */
	return "rgba";
}

static int conference_video_label_text_width(const char *text, const conference_video_label_style_t *style)
{
	const unsigned char *cursor = (const unsigned char *)text;
	int width = 0;

	if (zstr(text)) {
		return 0;
	}

	while (*cursor) {
		if (*cursor & 0x80) {
			width += style->text_width;
			cursor++;
			while (*cursor && ((*cursor & 0xC0) == 0x80)) {
				cursor++;
			}
			continue;
		}

		if (*cursor == ' ') {
			width += style->space_width;
		} else {
			width += style->text_width;
		}

		cursor++;
	}

	return width;
}

int conference_video_label_text_width_for_size(const char *text, int width, int height)
{
	conference_video_label_style_t style = conference_video_label_style(width, height);

	return conference_video_label_text_width(text, &style);
}

/**
 * 为视频层初始化 FFmpeg filter（在左下角显示用户ID/名称，背景透明）
 * @param layer 视频层对象
 * @param member 会议成员对象
 * @return SWITCH_STATUS_SUCCESS 成功，SWITCH_STATUS_FALSE 失败
 */
switch_status_t conference_video_init_layer_filter(mcu_layer_t *layer, conference_member_t *member, int width, int height)
{
	const AVFilter *buffersrc  = avfilter_get_by_name("buffer");
	const AVFilter *buffersink = avfilter_get_by_name("buffersink");
	AVFilterInOut *outputs = NULL;
	AVFilterInOut *inputs  = NULL;
	enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE };
	char args[512];
	char filter_desc[2048];
	char user_text[256];
	char escaped_text[512];
	const char *mic_image_path = NULL;
	int ret;
	int font_size;
	int label_text_x;
	int label_top;
	int icon_top;
	conference_video_label_style_t label_style;
	switch_channel_t *channel = NULL;
	const char *caller_id_name = NULL;
	switch_bool_t has_mic_icon = SWITCH_FALSE;

	if (!layer || !member || !member->session || width <= 0 || height <= 0) {
		return SWITCH_STATUS_FALSE;
	}

	if (!member->conference || member->conference->video_fps.samples <= 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Invalid video fps for member %d\n", member->id);
		return SWITCH_STATUS_FALSE;
	}

	/* 完全禁用 FFmpeg 日志输出 */
	av_log_set_level(AV_LOG_ERROR);

	/* 获取用户名称或ID - 优先使用member自身的channel */
	channel = member->channel;

	/* 尝试获取caller_id_name */
	if (channel) {  //conference_auto_outcall_caller_id_name
		caller_id_name = switch_channel_get_variable(channel, "conference_auto_outcall_caller_id_name");

		if (zstr(caller_id_name)) {
			caller_id_name = switch_channel_get_variable(channel, "caller_id_name");
		}
	}

	if (zstr(caller_id_name)) {
		/* 如果没有名称，使用成员ID */
		snprintf(user_text, sizeof(user_text), "User %d", member->id);
	} else {
		snprintf(user_text, sizeof(user_text), "%s", caller_id_name);
	}

	label_style = conference_video_label_style(width, height);
	font_size = conference_video_label_font_size(&label_style);

	/* 销毁已存在的 filter */
	conference_video_destroy_layer_filter(layer);

	/* 创建 filter graph */
	layer->filter_graph = avfilter_graph_alloc();
	if (!layer->filter_graph) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, 
			"Failed to allocate filter graph for member %d\n", member->id);
		return SWITCH_STATUS_FALSE;
	}

	/* 配置 buffer source (输入) */
	snprintf(args, sizeof(args),
		"video_size=%dx%d:pix_fmt=%d:time_base=1/90000:pixel_aspect=1/1",
		width, height, AV_PIX_FMT_YUV420P);

	ret = avfilter_graph_create_filter(&layer->buffersrc_ctx, buffersrc, "in",
		args, NULL, layer->filter_graph);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Cannot create buffer source for member %d: %d\n", member->id, ret);
		goto error;
	}

	/* 配置 buffer sink (输出) */
	ret = avfilter_graph_create_filter(&layer->buffersink_ctx, buffersink, "out",
		NULL, NULL, layer->filter_graph);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Cannot create buffer sink for member %d: %d\n", member->id, ret);
		goto error;
	}

	ret = av_opt_set_int_list(layer->buffersink_ctx, "pix_fmts", pix_fmts,
		AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Cannot set output pixel format for member %d: %d\n", member->id, ret);
		goto error;
	}


	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"Member %d video resolution: %dx%d\n", member->id, width, height);


	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
		"Initializing video filter for member %d (%s)\n", member->id, user_text);
	/* 获取麦克风图标路径：根据当前静音状态选择不同图标 */
	{
		switch_bool_t can_speak = conference_utils_member_test_flag(member, MFLAG_CAN_SPEAK);
		const char *custom_on  = NULL;
		const char *custom_off = NULL;

		if (channel) {
			custom_on  = switch_channel_get_variable(channel, "conference_video_mic_on_image");
			custom_off = switch_channel_get_variable(channel, "conference_video_mic_off_image");
		}

		if (can_speak) {
			mic_image_path = zstr(custom_on)  ? "/usr/local/freeswitch/share/freeswitch/images/mic_on.png"  : custom_on;
		} else {
			mic_image_path = zstr(custom_off) ? "/usr/local/freeswitch/share/freeswitch/images/mic_off.png" : custom_off;
		}

		/* 记录当前静音状态，供 apply 函数检测变化 */
		layer->filter_last_can_speak = can_speak;
	}

	/*
	 * 对 user_text 做 FFmpeg drawtext 转义：
	 *   '  ->  \'   （单引号，drawtext text='...' 内部需转义）
	 *   :  ->  \:    （冒号是 FFmpeg 选项分隔符）
	 *   \  ->  \\   （反斜杠）
	 * 先做反斜杠，再做冒号，最后做单引号，顺序不可颠倒。
	 */
	{
		const char *src = user_text;
		char *dst = escaped_text;
		char *dst_end = escaped_text + sizeof(escaped_text) - 4;
		while (*src && dst < dst_end) {
			if (*src == '\\') {
				*dst++ = '\\'; *dst++ = '\\';
			} else if (*src == ':') {
				*dst++ = '\\'; *dst++ = ':';
			} else if (*src == '\'') {
				*dst++ = '\\'; *dst++ = '\\'; *dst++ = '\\'; *dst++ = '\'';
			} else {
				*dst++ = *src;
			}
			src++;
		}
		*dst = '\0';
	}

	has_mic_icon = (switch_file_exists(mic_image_path, NULL) == SWITCH_STATUS_SUCCESS);
	label_top = conference_video_label_top_from_style(height, &label_style);
	icon_top = conference_video_label_icon_top_from_style(height, &label_style);
	label_text_x = label_style.x + label_style.padding_x;
	if (has_mic_icon) {
		label_text_x += label_style.icon_size + label_style.icon_gap;
	}

	/*
	 * 配置 filter：在左下角叠加麦克风图标和用户名称
	 *
	 * 带图标的 filter chain：
	 *   movie（加载 PNG）,scale=16:16 -> [mic]
	 *   [in][mic] -> overlay（图标放入左下角）-> [label_with_icon]
	 *   [label_with_icon] -> drawtext（文字放在图标右侧）-> [out]
	 *
	 * 统一标签方案：
	 *   - 样式按当前画面尺寸按比例缩放，小布局时标签会同步缩小
	 *   - 背景保持透明，不再绘制共享底板
	 *   - 数字、中文、字母统一使用同一套字号策略
	 *   - 图标、内边距、字号和文字宽度估算都使用同一缩放比例
	 *   - 所有非空格字符基于同一宽度规则估算标签宽度
	 *   - 大于基准尺寸时保持原有大小，不做额外放大
	 *
	 * 文件不存在时仅显示纯文字。
	 */
	if (has_mic_icon) {
		snprintf(filter_desc, sizeof(filter_desc),
			"movie='%s',scale=%d:%d,format=%s[mic];"
			"[in][mic]overlay=%d:%d[label_with_icon];"
			"[label_with_icon]drawtext=text='%s':fontfile=/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc:"
			"x=%d:y=%d+(%d-text_h)/2:fontsize=%d:fontcolor=white[out]",
			mic_image_path,
			label_style.icon_size,
			label_style.icon_size,
			conference_video_mic_icon_pixel_format(),
			label_style.x + label_style.padding_x,
			icon_top,
			escaped_text,
			label_text_x,
			label_top,
			label_style.label_height,
			font_size);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"Member %d: mic icon overlay enabled (%s)\n", member->id, mic_image_path);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"Member %d: mic icon not found at '%s', using text-only filter\n",
			member->id, mic_image_path);
		snprintf(filter_desc, sizeof(filter_desc),
			"drawtext=text='%s':fontfile=/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc:"
			"x=%d:y=%d+(%d-text_h)/2:fontsize=%d:fontcolor=white[out]",
			escaped_text,
			label_text_x,
			label_top,
			label_style.label_height,
			font_size);
	}
	/* 创建 filter graph 输入输出端点 */
	outputs = avfilter_inout_alloc();
	inputs  = avfilter_inout_alloc();
	if (!outputs || !inputs) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to allocate filter inout for member %d\n", member->id);
		goto error;
	}

	outputs->name       = av_strdup("in");
	outputs->filter_ctx = layer->buffersrc_ctx;
	outputs->pad_idx    = 0;
	outputs->next       = NULL;

	inputs->name       = av_strdup("out");
	inputs->filter_ctx = layer->buffersink_ctx;
	inputs->pad_idx    = 0;
	inputs->next       = NULL;

	/* 解析 filter chain */
	ret = avfilter_graph_parse_ptr(layer->filter_graph, filter_desc,&inputs, &outputs, NULL);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to parse filter graph for member %d: %d\n", member->id, ret);
		goto error;
	}

	/* 验证并初始化 filter graph */
	ret = avfilter_graph_config(layer->filter_graph, NULL);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to configure filter graph for member %d: %d\n", member->id, ret);
		goto error;
	}

	/* 分配 AVFrame */
	layer->filter_frame_in = av_frame_alloc();
	layer->filter_frame_out = av_frame_alloc();
	if (!layer->filter_frame_in || !layer->filter_frame_out) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to allocate AVFrame for member %d\n", member->id);
		goto error;
	}

	layer->filter_pts = 0;
	layer->filter_pts_step = member->conference->video_fps.samples;
	layer->filter_enabled = SWITCH_TRUE;
	layer->filter_w = width;
	layer->filter_h = height;

	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
		"Video filter initialized for member %d (%s) at %dx%d\n", 
		member->id, user_text, width, height);

	return SWITCH_STATUS_SUCCESS;

error:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	conference_video_destroy_layer_filter(layer);
	return SWITCH_STATUS_FALSE;
}

/**
 * 销毁视频层的 filter
 * @param layer 视频层对象
 */
void conference_video_destroy_layer_filter(mcu_layer_t *layer)
{
	if (!layer) {
		return;
	}

	if (layer->filter_graph) {
		avfilter_graph_free(&layer->filter_graph);
		layer->filter_graph = NULL;
	}

	layer->buffersrc_ctx = NULL;
	layer->buffersink_ctx = NULL;

	if (layer->filter_frame_in) {
		av_frame_free(&layer->filter_frame_in);
		layer->filter_frame_in = NULL;
	}

	if (layer->filter_frame_out) {
		av_frame_free(&layer->filter_frame_out);
		layer->filter_frame_out = NULL;
	}

	layer->filter_w = 0;
	layer->filter_h = 0;
	layer->filter_pts = 0;
	layer->filter_pts_step = 0;

	layer->filter_enabled = SWITCH_FALSE;
}

/**
 * 对视频帧应用 filter（在左下角添加用户ID/名称）
 * @param layer 视频层对象
 * @param img 输入/输出图像指针（处理后会替换原图像）
 * @return SWITCH_STATUS_SUCCESS 成功，SWITCH_STATUS_FALSE 失败
 */
switch_status_t conference_video_apply_layer_filter(mcu_layer_t *layer, switch_image_t **img)
{
	int ret, i;
	switch_image_t *new_img = NULL;

	if (!layer || !img || !*img || !layer->filter_enabled || !layer->filter_graph) {
		return SWITCH_STATUS_FALSE;
	}

	if (layer->filter_w != (*img)->d_w || layer->filter_h != (*img)->d_h) {
		switch_status_t reinit_status = SWITCH_STATUS_FALSE;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"Reinitializing video filter due to size change: %dx%d -> %dx%d\n",
			layer->filter_w, layer->filter_h, (*img)->d_w, (*img)->d_h);

		conference_video_destroy_layer_filter(layer);
		if (layer->member) {
			reinit_status = conference_video_init_layer_filter(layer, layer->member, (*img)->d_w, (*img)->d_h);
		}

		if (reinit_status != SWITCH_STATUS_SUCCESS) {
			return SWITCH_STATUS_FALSE;
		}
	}

	/* 检测静音状态变化，变化时重建 filter graph 以切换图标 */
	if (layer->member) {
		switch_bool_t can_speak_now = conference_utils_member_test_flag(layer->member, MFLAG_CAN_SPEAK);
		if (can_speak_now != layer->filter_last_can_speak) {
			switch_status_t reinit_status;

			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"Member %d mute state changed (%s), reinitializing video filter\n",
				layer->member->id, can_speak_now ? "unmuted" : "muted");

			conference_video_destroy_layer_filter(layer);
			reinit_status = conference_video_init_layer_filter(
				layer, layer->member, (*img)->d_w, (*img)->d_h);

			if (reinit_status != SWITCH_STATUS_SUCCESS) {
				return SWITCH_STATUS_FALSE;
			}
		}
	}

	/* 设置输入 AVFrame 参数并分配缓冲区 */
	layer->filter_frame_in->format = AV_PIX_FMT_YUV420P;
	layer->filter_frame_in->width  = (*img)->d_w;
	layer->filter_frame_in->height = (*img)->d_h;
	layer->filter_frame_in->pts = layer->filter_pts;
	layer->filter_pts += layer->filter_pts_step;

	/* 分配 AVFrame 缓冲区 */
	ret = av_frame_get_buffer(layer->filter_frame_in, 32);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to allocate AVFrame buffer: %d\n", ret);
		return SWITCH_STATUS_FALSE;
	}

	/* 逐行复制 Y 平面数据到 AVFrame */
	for (i = 0; i < (*img)->d_h; i++) {
		memcpy(layer->filter_frame_in->data[0] + i * layer->filter_frame_in->linesize[0],
			   (*img)->planes[SWITCH_PLANE_Y] + i * (*img)->stride[SWITCH_PLANE_Y],
			   (*img)->d_w);
	}

	/* 逐行复制 U 平面数据到 AVFrame */
	for (i = 0; i < (*img)->d_h / 2; i++) {
		memcpy(layer->filter_frame_in->data[1] + i * layer->filter_frame_in->linesize[1],
			   (*img)->planes[SWITCH_PLANE_U] + i * (*img)->stride[SWITCH_PLANE_U],
			   (*img)->d_w / 2);
	}

	/* 逐行复制 V 平面数据到 AVFrame */
	for (i = 0; i < (*img)->d_h / 2; i++) {
		memcpy(layer->filter_frame_in->data[2] + i * layer->filter_frame_in->linesize[2],
			   (*img)->planes[SWITCH_PLANE_V] + i * (*img)->stride[SWITCH_PLANE_V],
			   (*img)->d_w / 2);
	}

	/* 推送帧到 filter graph */
	ret = av_buffersrc_add_frame_flags(layer->buffersrc_ctx, layer->filter_frame_in,
		AV_BUFFERSRC_FLAG_KEEP_REF);
	if (ret < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
			"Error feeding frame to filter graph: %d\n", ret);
		av_frame_unref(layer->filter_frame_in);
		return SWITCH_STATUS_FALSE;
	}

	/* 释放输入帧缓冲区 */
	av_frame_unref(layer->filter_frame_in);

	/* 从 filter graph 获取处理后的帧 */
	ret = av_buffersink_get_frame(layer->buffersink_ctx, layer->filter_frame_out);
	if (ret < 0) {
		if (ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG,
				"Error getting filtered frame: %d\n", ret);
		}
		return SWITCH_STATUS_FALSE;
	}

	/* 创建新的 switch_image_t 并复制过滤后的数据 */
	new_img = switch_img_alloc(NULL, SWITCH_IMG_FMT_I420, 
		layer->filter_frame_out->width, layer->filter_frame_out->height, 1);
	if (!new_img) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
			"Failed to allocate output image\n");
		av_frame_unref(layer->filter_frame_out);
		return SWITCH_STATUS_FALSE;
	}

	/* 逐行复制 Y 平面（从 AVFrame 到 switch_image_t） */
	for (i = 0; i < layer->filter_frame_out->height; i++) {
		memcpy(new_img->planes[SWITCH_PLANE_Y] + i * new_img->stride[SWITCH_PLANE_Y],
			   layer->filter_frame_out->data[0] + i * layer->filter_frame_out->linesize[0],
			   layer->filter_frame_out->width);
	}

	/* 逐行复制 U 平面 */
	for (i = 0; i < layer->filter_frame_out->height / 2; i++) {
		memcpy(new_img->planes[SWITCH_PLANE_U] + i * new_img->stride[SWITCH_PLANE_U],
			   layer->filter_frame_out->data[1] + i * layer->filter_frame_out->linesize[1],
			   layer->filter_frame_out->width / 2);
	}

	/* 逐行复制 V 平面 */
	for (i = 0; i < layer->filter_frame_out->height / 2; i++) {
		memcpy(new_img->planes[SWITCH_PLANE_V] + i * new_img->stride[SWITCH_PLANE_V],
			   layer->filter_frame_out->data[2] + i * layer->filter_frame_out->linesize[2],
			   layer->filter_frame_out->width / 2);
	}

	/* 释放旧图像并替换为新图像 */
	switch_img_free(img);
	*img = new_img;

	av_frame_unref(layer->filter_frame_out);

	return SWITCH_STATUS_SUCCESS;
}

#endif /* HAVE_LIBAVFILTER */

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
