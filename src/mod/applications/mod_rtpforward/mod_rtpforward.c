/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * mod_rtpforward - Forward RTP streams from conference participants to VLC
 *
 * Copyright (C) 2025
 *
 * This module captures audio/video from conference participants using media_bug
 * and forwards them as RTP streams to external destinations (e.g., VLC)
 */
#include "ws_client.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* Module interface */
SWITCH_MODULE_LOAD_FUNCTION(mod_rtpforward_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rtpforward_shutdown);
SWITCH_MODULE_DEFINITION(mod_rtpforward, mod_rtpforward_load, mod_rtpforward_shutdown, NULL);

/* Global module settings instance (definition) */
struct globals_s globals;

/* Forward declarations */
static void destroy_forward_context(rtpforward_context_t *context);

static void log_session_media_info(switch_core_session_t *session, rtpforward_context_t *context);

static switch_status_t rtpforward_on_hangup(switch_core_session_t *session);

static switch_status_t rtpforward_on_destroy(switch_core_session_t *session);

static switch_status_t rtpforward_on_exchange_media(switch_core_session_t *session);
/* State handler table */
static switch_state_handler_table_t rtpforward_state_handlers = {.on_hangup = rtpforward_on_hangup,
																 .on_destroy = rtpforward_on_destroy,
																 .on_exchange_media = rtpforward_on_exchange_media};

/* State handler callbacks */
static switch_status_t rtpforward_on_hangup(switch_core_session_t *session) {
	switch_channel_t *channel = switch_core_session_get_channel(session);
	const char *uuid = switch_core_session_get_uuid(session);
	rtpforward_context_t *context = NULL;

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
					  "Session %s hangup detected, cleaning up RTP forwarding\n", uuid);

	/* Find and remove context from global hash */
	switch_mutex_lock(globals.mutex);
	context = (rtpforward_context_t *)switch_core_hash_find(globals.hash_table, uuid);
	if (context) { switch_core_hash_delete(globals.hash_table, uuid); }
	switch_mutex_unlock(globals.mutex);

	/* Clean up resources */
	if (context) {
		/* 通过 WebSocket 发送挂断通知 */
		if (context->ws_client) { ws_client_send_context_json(context->ws_client, context, "rtpforward_hangup"); }

		/* 移除状态处理器，避免重复触发 */
		switch_channel_clear_state_handler(channel, &rtpforward_state_handlers);

		/* 清理转发上下文（会移除 media bugs、关闭 socket） */
		destroy_forward_context(context);

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "RTP forwarding resources cleaned up for session %s\n", uuid);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtpforward_on_destroy(switch_core_session_t *session) {

	const char *uuid = switch_core_session_get_uuid(session);
	rtpforward_context_t *context = NULL;

	/* 通常 on_hangup 已经清理过了，这里作为兜底检查 */
	switch_mutex_lock(globals.mutex);
	context = (rtpforward_context_t *)switch_core_hash_find(globals.hash_table, uuid);
	if (context) {
		switch_core_hash_delete(globals.hash_table, uuid);
		switch_mutex_unlock(globals.mutex);

		/* 清理转发上下文 */
		destroy_forward_context(context);

		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_WARNING,
						  "RTP forwarding cleanup in on_destroy for session %s (should have been done in on_hangup)\n",
						  uuid);
	} else {
		switch_mutex_unlock(globals.mutex);
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG,
						  "Session %s destroy - no cleanup needed (already cleaned)\n", uuid);
	}

	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t rtpforward_on_exchange_media(switch_core_session_t *session) {

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Media exchange/renegotiation detected\n");
	/* 打印更新后的媒体信息 */
	// log_session_media_info(session);

	return SWITCH_STATUS_SUCCESS;
}

// /* State handler table */
// static switch_state_handler_table_t rtpforward_state_handlers = {.on_hangup = rtpforward_on_hangup,
// 																 .on_destroy = rtpforward_on_destroy,
// 																 .on_exchange_media = rtpforward_on_exchange_media};

/* Initialize RTP socket with non-blocking mode */
static switch_status_t init_rtp_socket(int *sock, struct sockaddr_in *addr, const char *dest_ip, int port) {
	int flags;
	int sndbuf = 524288; /* 512KB send buffer */

	*sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (*sock < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create socket\n");
		return SWITCH_STATUS_FALSE;
	}

	/* Set non-blocking mode */
	flags = fcntl(*sock, F_GETFL, 0);
	fcntl(*sock, F_SETFL, flags | O_NONBLOCK);

	/* Increase send buffer */
	setsockopt(*sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

	memset(addr, 0, sizeof(struct sockaddr_in));
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);

	if (inet_pton(AF_INET, dest_ip, &addr->sin_addr) <= 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid IP address: %s\n", dest_ip);
		close(*sock);
		return SWITCH_STATUS_FALSE;
	}

	return SWITCH_STATUS_SUCCESS;
}

#ifdef _DEBUGFORWARD
static const char *abc_type_to_string(switch_abc_type_t type) {
	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		return "SWITCH_ABC_TYPE_INIT";
	case SWITCH_ABC_TYPE_READ:
		return "SWITCH_ABC_TYPE_READ";
	case SWITCH_ABC_TYPE_WRITE:
		return "SWITCH_ABC_TYPE_WRITE";
	case SWITCH_ABC_TYPE_WRITE_REPLACE:
		return "SWITCH_ABC_TYPE_WRITE_REPLACE";
	case SWITCH_ABC_TYPE_READ_REPLACE:
		return "SWITCH_ABC_TYPE_READ_REPLACE";
	case SWITCH_ABC_TYPE_READ_PING:
		return "SWITCH_ABC_TYPE_READ_PING";
	case SWITCH_ABC_TYPE_TAP_NATIVE_READ:
		return "SWITCH_ABC_TYPE_TAP_NATIVE_READ";
	case SWITCH_ABC_TYPE_TAP_NATIVE_WRITE:
		return "SWITCH_ABC_TYPE_TAP_NATIVE_WRITE";
	case SWITCH_ABC_TYPE_CLOSE:
		return "SWITCH_ABC_TYPE_CLOSE";
	case SWITCH_ABC_TYPE_READ_VIDEO_PING:
		return "SWITCH_ABC_TYPE_READ_VIDEO_PING";
	case SWITCH_ABC_TYPE_WRITE_VIDEO_PING:
		return "SWITCH_ABC_TYPE_WRITE_VIDEO_PING";
	case SWITCH_ABC_TYPE_STREAM_VIDEO_PING:
		return "SWITCH_ABC_TYPE_STREAM_VIDEO_PING";
	case SWITCH_ABC_TYPE_VIDEO_PATCH:
		return "SWITCH_ABC_TYPE_VIDEO_PATCH";
	case SWITCH_ABC_TYPE_READ_TEXT:
		return "SWITCH_ABC_TYPE_READ_TEXT";
	default:
		return "UNKNOWN";
	}
}
#endif

/* Audio media bug callback */
static switch_bool_t audio_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type) {
	rtpforward_context_t *context = (rtpforward_context_t *)user_data;
	// switch_frame_t *frame;

	// char buffer[256];
	// switch_snprintf(buffer, sizeof(buffer), " audio_bug_callback:Media bug type: %s", abc_type_to_string(type));

	switch (type) {
	case SWITCH_ABC_TYPE_INIT:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Audio media bug initialized\n");
		context->audio_enabled = SWITCH_TRUE;
		break;
	case SWITCH_ABC_TYPE_TAP_NATIVE_READ: {
		if (context->running && context->audio_sock >= 0 && context->audio_enabled) {

			switch_frame_t *frame = switch_core_media_bug_get_native_read_frame(bug);
			// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
			//          "[AUDIO_BUG_CALLBACK] codec=%s, FS_samples=%u, FS_rate=%u, FS_channels=%u, FS_timestamp=%u,
			//          packetlen=%d, payload=%d\n", frame->codec ? frame->codec->implementation->iananame : "unknown",
			//          frame->samples,   /* 我们自己的序列号 */
			//          frame->rate,
			//          frame->channels,
			//          frame->timestamp,
			//          frame->packetlen,
			//          frame->payload);
			if (frame && frame->packetlen > 0) {
				/* 因为 frame 可能在回调后被复用，如果我们需要保存或异步发送，需要拷贝 */
				// uint8_t *payload_copy = switch_core_session_alloc(context->session, vf->packetlen);
				// memcpy(payload_copy, vf->packet, vf->packetlen);
				/* 立即发送（注意：send_video_rtp 内部未 free payload_copy） */
				sendto(context->audio_sock, frame->packet, frame->packetlen, MSG_DONTWAIT,
					   (struct sockaddr *)&context->audio_addr, sizeof(context->audio_addr));
			}
		}
	} break;
	case SWITCH_ABC_TYPE_READ_REPLACE:
		break;
	case SWITCH_ABC_TYPE_CLOSE:
		context->audio_enabled = SWITCH_FALSE;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Audio media bug closing\n");
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

/* Video media bug callback */
static switch_bool_t video_bug_callback(switch_media_bug_t *bug, void *user_data, switch_abc_type_t type) {

	rtpforward_context_t *context = (rtpforward_context_t *)user_data;
	// switch_snprintf(buffer, sizeof(buffer), " video_bug_callback:Media bug type: %s", abc_type_to_string(type));
	// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, " video_bug_callback: Media bug type: %d\n", type);
	switch (type) {
	case SWITCH_ABC_TYPE_INIT: {
		// const char *uuid;

		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Video media bug initialized\n");
		context->video_enabled = SWITCH_TRUE;
		switch_core_session_request_video_refresh(context->session);
		switch_core_media_gen_key_frame(context->session);
#ifdef _DEBUGFORWARD
		switch_core_session_t *session = switch_core_media_bug_get_session(bug);
		// /* 打开文件用于保存 H.264 裸流 */
		uuid = switch_core_session_get_uuid(session);
		switch_snprintf(context->video_file_path, sizeof(context->video_file_path), "/tmp/freeswitch_video_%s.h264",
						uuid);
		context->video_file = fopen(context->video_file_path, "wb");
		if (context->video_file) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "*** Saving H.264 stream to: %s ***\n",
							  context->video_file_path);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to open video file: %s\n",
							  context->video_file_path);
		}

		/* 立即请求关键帧（I帧）以获取 SPS/PPS/IDR */
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
						  "Requesting keyframe (I-frame) for video stream initialization\n");
		switch_core_session_request_video_refresh(session);
		switch_core_media_gen_key_frame(session);
#endif
	} break;
	case SWITCH_ABC_TYPE_READ_VIDEO_PING: {
		/* Capture raw H.264 RTP packets - this includes I-frames */
	} break;
	case SWITCH_ABC_TYPE_READ_VIDEO_RAW: {
		/* 取回回调的 frame 指针 */
		// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Video media bug READ_VIDEO_RAW callback\n");
		if (context->running && context->video_sock >= 0 && context->video_enabled) {
			switch_frame_t *vf = switch_core_media_bug_get_video_ping_frame(bug);
			if (!vf) break;
			if (vf->packet && vf->packetlen > 12) {
				/* 因为 frame 可能在回调后被复用，如果我们需要保存或异步发送，需要拷贝 */
				// uint8_t *payload_copy = switch_core_session_alloc(context->session, vf->packetlen);
				// memcpy(payload_copy, vf->packet, vf->packetlen);
				/* 立即发送（注意：send_video_rtp 内部未 free payload_copy） */
				sendto(context->video_sock, vf->packet, vf->packetlen, MSG_DONTWAIT,
					   (struct sockaddr *)&context->video_addr, sizeof(context->video_addr));
			}
		}
	} break;
	case SWITCH_ABC_TYPE_CLOSE:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Video media bug closing\n");
		context->video_enabled = SWITCH_FALSE;
#ifdef _DEBUGFORWARD
		/* 关闭视频文件 */
		if (context->video_file) {
			fclose(context->video_file);
			context->video_file = NULL;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "*** H.264 stream saved to: %s ***\n",
							  context->video_file_path);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "*** To play: vlc %s ***\n",
							  context->video_file_path);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "*** Or: ffplay %s ***\n",
							  context->video_file_path);
		}
#endif
		break;
	case SWITCH_ABC_TYPE_READ:
	case SWITCH_ABC_TYPE_WRITE:
		/* 不处理这些类型 */
		break;
	default:
		break;
	}

	return SWITCH_TRUE;
}

/* 获取并打印 session 的媒体信息 */
static void log_session_media_info(switch_core_session_t *session, rtpforward_context_t *context) {
	switch_channel_t *channel;
	switch_codec_t *audio_read_codec, *audio_write_codec;
	switch_codec_t *video_read_codec, *video_write_codec;
	const char *uuid;

	if (!session && !context) { return; }

	channel = switch_core_session_get_channel(session);
	uuid = switch_core_session_get_uuid(session);

	/* 获取音频编解码器 */
	audio_read_codec = switch_core_session_get_read_codec(session);
	audio_write_codec = switch_core_session_get_write_codec(session);

	/* 获取视频编解码器 */
	video_read_codec = switch_core_session_get_video_read_codec(session);
	video_write_codec = switch_core_session_get_video_write_codec(session);

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "=== Session Media Information ===\n");
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "UUID: %s\n", uuid);

	/* 音频信息 */
	if (audio_read_codec && audio_read_codec->implementation) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "Audio Read Codec: %s/%s @ %u Hz, %u channels, %u ms\n",
						  audio_read_codec->implementation->iananame,
						  audio_read_codec->implementation->ianacode
							  ? switch_core_session_sprintf(session, "%d", audio_read_codec->implementation->ianacode)
							  : "dynamic",
						  audio_read_codec->implementation->actual_samples_per_second,
						  audio_read_codec->implementation->number_of_channels,
						  audio_read_codec->implementation->microseconds_per_packet / 1000);

		/* 安全复制编解码器信息到 context */

		switch_snprintf(context->audio_codec, sizeof(context->audio_codec), "%s",
						audio_read_codec->implementation->iananame);
		context->audio_sample_rate = audio_read_codec->implementation->actual_samples_per_second;
		context->audio_channels = audio_read_codec->implementation->number_of_channels;
		context->audio_pt = audio_read_codec->implementation->ianacode;
		context->is_audio = SWITCH_TRUE;

	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Audio Read Codec: NONE\n");
		switch_set_string(context->audio_codec, "NONE");
	}

	if (audio_write_codec && audio_write_codec->implementation) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
						  "Audio Write Codec: %s/%s @ %u Hz, %u channels, %u ms\n",
						  audio_write_codec->implementation->iananame,
						  audio_write_codec->implementation->ianacode
							  ? switch_core_session_sprintf(session, "%d", audio_write_codec->implementation->ianacode)
							  : "dynamic",
						  audio_write_codec->implementation->actual_samples_per_second,
						  audio_write_codec->implementation->number_of_channels,
						  audio_write_codec->implementation->microseconds_per_packet / 1000);
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Audio Write Codec: NONE\n");
	}

	/* 视频信息 */
	if (switch_channel_test_flag(channel, CF_VIDEO)) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Video: ENABLED\n");

		if (video_read_codec && video_read_codec->implementation) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Video Read Codec: %s/%s @ %u Hz\n",
				video_read_codec->implementation->iananame,
				video_read_codec->implementation->ianacode
					? switch_core_session_sprintf(session, "%d", video_read_codec->implementation->ianacode)
					: "dynamic",
				video_read_codec->implementation->actual_samples_per_second);

			/* 安全复制视频编解码器信息到 context */
			switch_snprintf(context->video_codec, sizeof(context->video_codec), "%s",
							video_read_codec->implementation->iananame);
			context->video_sample_rate = video_read_codec->implementation->actual_samples_per_second;
			context->video_pt = video_read_codec->implementation->ianacode;
			context->is_video = SWITCH_TRUE;
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
							  "Video Read Codec: NONE (negotiating or not started)\n");
			if (context) { switch_set_string(context->video_codec, "NONE"); }
		}

		if (video_write_codec && video_write_codec->implementation) {
			switch_log_printf(
				SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Video Write Codec: %s/%s @ %u Hz\n",
				video_write_codec->implementation->iananame,
				video_write_codec->implementation->ianacode
					? switch_core_session_sprintf(session, "%d", video_write_codec->implementation->ianacode)
					: "dynamic",
				video_write_codec->implementation->actual_samples_per_second);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO,
							  "Video Write Codec: NONE (negotiating or not started)\n");
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "Video: DISABLED\n");
	}

	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_INFO, "================================\n");
}

/* Cleanup forward context */
static void destroy_forward_context(rtpforward_context_t *context) {
	if (!context) return;

	context->running = SWITCH_FALSE;

	if (context->audio_sock >= 0) {
		close(context->audio_sock);
		context->audio_sock = -1;
	}

	if (context->video_sock >= 0) {
		close(context->video_sock);
		context->video_sock = -1;
	}

	if (context->audio_bug) {
		switch_core_media_bug_remove(context->session, &context->audio_bug);
		context->audio_bug = NULL;
	}

	if (context->video_bug) {
		switch_core_media_bug_remove(context->session, &context->video_bug);
		context->video_bug = NULL;
	}
#ifdef _DEBUGFORWARD
	/* Close video file if still open */
	if (context->video_file) {
		fclose(context->video_file);
		context->video_file = NULL;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Video file closed: %s\n", context->video_file_path);
	}
#endif
	/* WebSocket 客户端是全局的，不在这里销毁 */
	context->ws_client = NULL;
}

/* Start forwarding for a session */
SWITCH_STANDARD_API(rtpforward_start_function) {
	char *argv[10] = {0};
	int argc;
	char *myarg = NULL;
	switch_core_session_t *psession = NULL;
	rtpforward_context_t *context = NULL;
	const char *uuid;
	char *dest_ip;
	int audio_port, video_port;
	switch_channel_t *channel;

	if (zstr(cmd)) {
		stream->write_function(stream, "-USAGE: rtpforward_start <uuid> <dest_ip> <audio_port> <video_port>\n");
		return SWITCH_STATUS_SUCCESS;
	}

	myarg = strdup(cmd);
	argc = switch_separate_string(myarg, ' ', argv, (sizeof(argv) / sizeof(argv[0])));

	if (argc < 4) {
		stream->write_function(stream, "-ERR Missing parameters\n");
		switch_safe_free(myarg);
		return SWITCH_STATUS_SUCCESS;
	}

	uuid = argv[0];
	dest_ip = argv[1];
	audio_port = atoi(argv[2]);
	video_port = atoi(argv[3]);

	if (!(psession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session\n");
		goto done;
	}

	channel = switch_core_session_get_channel(psession);

	/* 打印 session 的媒体信息 */
	// log_session_media_info(psession);

	/* Create context */
	context = switch_core_session_alloc(psession, sizeof(*context));
	memset(context, 0, sizeof(*context));

	context->session = psession;
	context->uuid = uuid;
	context->pool = switch_core_session_get_pool(psession);
	context->audio_sock = -1;
	context->video_sock = -1;

	switch_snprintf(context->dest_ip, sizeof(context->dest_ip), "%s", dest_ip);
	context->audio_port = audio_port;
	context->video_port = video_port;

	switch_mutex_init(&context->mutex, SWITCH_MUTEX_NESTED, context->pool);

	/* 使用全局 WebSocket 客户端 */
	context->ws_client = globals.ws_client;
	if (context->ws_client) {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
						  "Session %s will use global WebSocket client\n", uuid);
	}

	/* Initialize sockets */
	if (init_rtp_socket(&context->audio_sock, &context->audio_addr, dest_ip, audio_port) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Failed to initialize audio socket\n");
		goto done;
	}

	if (init_rtp_socket(&context->video_sock, &context->video_addr, dest_ip, video_port) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Failed to initialize video socket\n");
		goto done;
	}

	context->running = SWITCH_TRUE;
	context->is_audio = SWITCH_FALSE;
	context->is_video = SWITCH_FALSE;

	log_session_media_info(psession, context);

	/* Attach audio media bug */
	if (switch_core_media_bug_add(psession, "rtpforward_audio", NULL, audio_bug_callback, context, 0,
								  SMBF_TAP_NATIVE_READ | SMBF_NO_PAUSE | SMBF_ONE_ONLY,
								  &context->audio_bug) != SWITCH_STATUS_SUCCESS) {
		stream->write_function(stream, "-ERR Failed to attach audio media bug\n");
		destroy_forward_context(context);
		goto done;
	}

	/* Check if video is available and attach video media bug */
	if (switch_channel_test_flag(channel, CF_VIDEO)) {
		if (switch_core_media_bug_add(psession, "rtpforward_video", NULL, video_bug_callback, context, 0,
									  SMBF_READ_VIDEO_RAW | SMBF_NO_PAUSE | SMBF_ONE_ONLY,
									  &context->video_bug) == SWITCH_STATUS_SUCCESS) {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
							  "Video media bug attached successfully with READ_VIDEO_PING flag\n");

			/* 主动请求一个关键帧 */
			// switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_NOTICE,
			// 				  "*** Requesting initial keyframe for RTP forwarding ***\n");
			switch_core_session_request_video_refresh(psession);
			switch_core_media_gen_key_frame(psession);
		} else {
			switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
							  "Failed to attach video media bug - video may not be available\n");
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
						  "Session does not have video enabled\n");
	}

	/* Store context in global hash */
	switch_mutex_lock(globals.mutex);
	switch_core_hash_insert(globals.hash_table, uuid, context);
	switch_mutex_unlock(globals.mutex);

	/* Add state handler to monitor hangup and destroy events */
	switch_channel_add_state_handler(channel, &rtpforward_state_handlers);
	switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
					  "State handler attached for automatic cleanup on hangup/destroy\n");

	/* 通过 WebSocket 发送 context 信息 */
	if (context->ws_client) {
		ws_client_send_context_json(context->ws_client, context, "rtpforward_started");
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
						  "Context information sent to WebSocket server\n");
	}

	stream->write_function(stream, "+OK RTP forwarding started\n");

	/* Generate SDP file with better video support */
	// {
	//     char sdp_path[512];
	//     FILE *sdp_file;
	//     time_t now = time(NULL);

	//     switch_snprintf(sdp_path, sizeof(sdp_path), "/tmp/freeswitch_rtp_%s.sdp", uuid);
	//     sdp_file = fopen(sdp_path, "w");
	//     if (sdp_file) {
	//         fprintf(sdp_file, "v=0\r\n");
	//         fprintf(sdp_file, "o=FreeSWITCH %lu %lu IN IP4 %s\r\n",
	//                (unsigned long)now, (unsigned long)now, dest_ip);
	//         fprintf(sdp_file, "s=FreeSWITCH RTP Stream\r\n");
	//         fprintf(sdp_file, "c=IN IP4 %s\r\n", dest_ip);
	//         fprintf(sdp_file, "t=0 0\r\n");

	//         /* Audio stream */
	//         fprintf(sdp_file, "m=audio %d RTP/AVP 0\r\n", audio_port);
	//         fprintf(sdp_file, "a=rtpmap:0 PCMU/8000\r\n");
	//         fprintf(sdp_file, "a=ptime:20\r\n");
	//         fprintf(sdp_file, "a=sendonly\r\n");

	//         /* Video stream */
	//         if (video_port > 0 && video_port != audio_port) {
	//             fprintf(sdp_file, "m=video %d RTP/AVP 96\r\n", video_port);
	//             fprintf(sdp_file, "a=rtpmap:96 H264/90000\r\n");
	//             fprintf(sdp_file, "a=fmtp:96 profile-level-id=42e01e;packetization-mode=1\r\n");
	//             fprintf(sdp_file, "a=sendonly\r\n");
	//         }

	//         fclose(sdp_file);

	//         switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
	//                         "RTP forwarding started:\n"
	//                         "  Audio: %s:%d\n"
	//                         "  Video: %s:%d\n"
	//                         "  SDP file: %s\n"
	//                         "  VLC command: vlc %s --network-caching=300\n",
	//                         dest_ip, audio_port, dest_ip, video_port,
	//                         sdp_path, sdp_path);

	//         stream->write_function(stream,
	//                              "+OK RTP forwarding started\n"
	//                              "SDP file: %s\n"
	//                              "VLC command: vlc %s --network-caching=300\n",
	//                              sdp_path, sdp_path);
	//     } else {
	//         switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_WARNING,
	//                         "Failed to create SDP file: %s\n", sdp_path);
	//         stream->write_function(stream, "+OK RTP forwarding started (no SDP file)\n");
	//     }
	// }
done:
	if (psession) { switch_core_session_rwunlock(psession); }

	switch_safe_free(myarg);
	return SWITCH_STATUS_SUCCESS;
}

/* Stop forwarding for a session */
SWITCH_STANDARD_API(rtpforward_stop_function) {

	switch_core_session_t *psession = NULL;
	rtpforward_context_t *context = NULL;
	switch_channel_t *channel = NULL;
	const char *uuid = cmd;

	if (zstr(uuid)) {
		stream->write_function(stream, "-USAGE: rtpforward_stop <uuid>\n");
		return SWITCH_STATUS_SUCCESS;
	}

	if (!(psession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session\n");
		return SWITCH_STATUS_SUCCESS;
	}

	channel = switch_core_session_get_channel(psession);

	/* Find and remove context */
	switch_mutex_lock(globals.mutex);
	context = (rtpforward_context_t *)switch_core_hash_find(globals.hash_table, uuid);
	if (context) { switch_core_hash_delete(globals.hash_table, uuid); }
	switch_mutex_unlock(globals.mutex);

	if (context) {
		// char sdp_path[512];

		/* 通过 WebSocket 发送停止通知 */
		context->uuid = uuid; // 确保 uuid 字段正确
		if (context->ws_client) { ws_client_send_context_json(context->ws_client, context, "rtpforward_stopped"); }

		/* 先移除状态处理器，避免在清理过程中触发回调 */
		switch_channel_clear_state_handler(channel, &rtpforward_state_handlers);

		/* 清理转发上下文（会移除 media bugs） */
		destroy_forward_context(context);

		/* Remove SDP file */
		// switch_snprintf(sdp_path, sizeof(sdp_path), "/tmp/freeswitch_rtp_%s.sdp", uuid);
		// unlink(sdp_path);
		stream->write_function(stream, "+OK RTP forwarding stopped\n");
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(psession), SWITCH_LOG_INFO,
						  "RTP forwarding stopped for session %s\n", uuid);
	} else {
		stream->write_function(stream, "-ERR No active forwarding found\n");
	}

	switch_core_session_rwunlock(psession);
	return SWITCH_STATUS_SUCCESS;
}

/* Get session media info */
SWITCH_STANDARD_API(rtpforward_info_function) {
	switch_core_session_t *psession = NULL;
	switch_channel_t *channel;
	switch_codec_t *audio_read_codec, *audio_write_codec;
	switch_codec_t *video_read_codec, *video_write_codec;
	const char *uuid = cmd;

	if (zstr(uuid)) {
		stream->write_function(stream, "-USAGE: rtpforward_info <uuid>\n");
		return SWITCH_STATUS_SUCCESS;
	}

	if (!(psession = switch_core_session_locate(uuid))) {
		stream->write_function(stream, "-ERR Cannot locate session\n");
		return SWITCH_STATUS_SUCCESS;
	}

	channel = switch_core_session_get_channel(psession);

	/* 获取编解码器 */
	audio_read_codec = switch_core_session_get_read_codec(psession);
	audio_write_codec = switch_core_session_get_write_codec(psession);
	video_read_codec = switch_core_session_get_video_read_codec(psession);
	video_write_codec = switch_core_session_get_video_write_codec(psession);

	stream->write_function(stream, "+OK\n");
	stream->write_function(stream, "UUID: %s\n", uuid);
	stream->write_function(stream, "\n");

	/* 音频信息 */
	stream->write_function(stream, "[Audio]\n");
	if (audio_read_codec && audio_read_codec->implementation) {
		stream->write_function(stream, "  Read Codec: %s (PT:%u)\n", audio_read_codec->implementation->iananame,
							   audio_read_codec->implementation->ianacode);
		stream->write_function(stream, "  Sample Rate: %u Hz\n",
							   audio_read_codec->implementation->actual_samples_per_second);
		stream->write_function(stream, "  Channels: %u\n", audio_read_codec->implementation->number_of_channels);
		stream->write_function(stream, "  Packet Time: %u ms\n",
							   audio_read_codec->implementation->microseconds_per_packet / 1000);
	} else {
		stream->write_function(stream, "  Read Codec: NONE\n");
	}

	if (audio_write_codec && audio_write_codec->implementation) {
		stream->write_function(stream, "  Write Codec: %s (PT:%u)\n", audio_write_codec->implementation->iananame,
							   audio_write_codec->implementation->ianacode);
	} else {
		stream->write_function(stream, "  Write Codec: NONE\n");
	}
	stream->write_function(stream, "\n");

	/* 视频信息 */
	stream->write_function(stream, "[Video]\n");
	if (switch_channel_test_flag(channel, CF_VIDEO)) {
		stream->write_function(stream, "  Status: ENABLED\n");

		if (video_read_codec && video_read_codec->implementation) {
			stream->write_function(stream, "  Read Codec: %s (PT:%u)\n", video_read_codec->implementation->iananame,
								   video_read_codec->implementation->ianacode);
			stream->write_function(stream, "  Sample Rate: %u Hz\n",
								   video_read_codec->implementation->actual_samples_per_second);
		} else {
			stream->write_function(stream, "  Read Codec: NONE (not negotiated yet)\n");
		}

		if (video_write_codec && video_write_codec->implementation) {
			stream->write_function(stream, "  Write Codec: %s (PT:%u)\n", video_write_codec->implementation->iananame,
								   video_write_codec->implementation->ianacode);
		} else {
			stream->write_function(stream, "  Write Codec: NONE (not negotiated yet)\n");
		}
	} else {
		stream->write_function(stream, "  Status: DISABLED\n");
	}

	switch_core_session_rwunlock(psession);
	return SWITCH_STATUS_SUCCESS;
}

/* List active forwarding sessions */
SWITCH_STANDARD_API(rtpforward_list_function) {
	switch_hash_index_t *hi;
	void *val;
	const void *key;
	switch_ssize_t keylen;
	int count = 0;

	stream->write_function(stream, "Active RTP Forwarding Sessions:\n");
	stream->write_function(stream, "================================\n");

	switch_mutex_lock(globals.mutex);
	for (hi = switch_core_hash_first(globals.hash_table); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &key, &keylen, &val);
		if (val) {
			rtpforward_context_t *ctx = (rtpforward_context_t *)val;
			stream->write_function(stream, "UUID: %s\n", (char *)key);
			stream->write_function(stream, "  Destination: %s\n", ctx->dest_ip);
			stream->write_function(stream, "  Audio Port: %d\n", ctx->audio_port);
			stream->write_function(stream, "  Video Port: %d\n", ctx->video_port);
			stream->write_function(stream, "  Video Enabled: %s\n", ctx->video_enabled ? "Yes" : "No");
			stream->write_function(stream, "\n");
			count++;
		}
	}
	switch_mutex_unlock(globals.mutex);

	if (count == 0) {
		stream->write_function(stream, "No active forwarding sessions.\n");
	} else {
		stream->write_function(stream, "Total: %d session(s)\n", count);
	}

	return SWITCH_STATUS_SUCCESS;
}

/* Module load function */
SWITCH_MODULE_LOAD_FUNCTION(mod_rtpforward_load) {
	switch_api_interface_t *api_interface;
	switch_status_t status;

	memset(&globals, 0, sizeof(globals));
	globals.pool = pool;

	switch_core_hash_init(&globals.hash_table);
	switch_mutex_init(&globals.mutex, SWITCH_MUTEX_NESTED, globals.pool);

	/* 加载 WebSocket 配置 */
	status = ws_config_load(&globals.ws_config);
	if (status != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to load configuration, module cannot start\n");
		switch_core_hash_destroy(&globals.hash_table);
		return status;
	}

	if (globals.ws_config.ws_enabled) {
		globals.ws_client = ws_client_create(&globals.ws_config, globals.pool);
		if (globals.ws_client) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Global WebSocket client created and connecting\n");
		} else if (globals.ws_config.ws_enabled) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
							  "WebSocket enabled but failed to create client\n");
		}
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket support is DISABLED\n");
	}

	// globals.ws_client = ws_client_create(&globals.ws_config, globals.pool);
	// if (globals.ws_client) {
	//     switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
	//                     "Global WebSocket client created and connecting\n");
	// } else if (globals.ws_config.ws_enabled) {
	//     switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
	//                     "WebSocket enabled but failed to create client\n");
	// }

	/* Connect module to interface registry */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	/* Register API commands */
	SWITCH_ADD_API(api_interface, "rtpforward_start", "Start RTP forwarding", rtpforward_start_function,
				   "<uuid> <dest_ip> <audio_port> <video_port>");
	SWITCH_ADD_API(api_interface, "rtpforward_stop", "Stop RTP forwarding", rtpforward_stop_function, "<uuid>");
	SWITCH_ADD_API(api_interface, "rtpforward_list", "List active forwarding sessions", rtpforward_list_function, "");
	SWITCH_ADD_API(api_interface, "rtpforward_info", "Get session media info", rtpforward_info_function, "<uuid>");

	switch_console_set_complete("add rtpforward_start");
	switch_console_set_complete("add rtpforward_stop");
	switch_console_set_complete("add rtpforward_list");
	switch_console_set_complete("add rtpforward_info");

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Module %s loaded successfully\n", modname);

	return SWITCH_STATUS_SUCCESS;
}

/* Module shutdown function */
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_rtpforward_shutdown) {
	switch_hash_index_t *hi;
	void *val;
	const void *key;
	switch_ssize_t keylen;

	/* Clean up all active contexts */
	switch_mutex_lock(globals.mutex);
	for (hi = switch_core_hash_first(globals.hash_table); hi; hi = switch_core_hash_next(&hi)) {
		switch_core_hash_this(hi, &key, &keylen, &val);
		if (val) { destroy_forward_context((rtpforward_context_t *)val); }
	}
	switch_core_hash_destroy(&globals.hash_table);
	switch_mutex_unlock(globals.mutex);

	/* 销毁全局 WebSocket 客户端 */
	if (globals.ws_client) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Destroying global WebSocket client\n");
		ws_client_destroy(globals.ws_client);
		globals.ws_client = NULL;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Module %s unloaded\n", modname);

	return SWITCH_STATUS_SUCCESS;
}