/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * ws_client.h -- WebSocket Client for RTP forwarding
 *
 * Copyright (C) 2025
 */

/****
 *
 *
 *
 *
 *
 */
#ifndef _WS_CLIENT_H_
#define _WS_CLIENT_H_

#include <switch.h>
#include <libwebsockets.h>

/* WebSocket 客户端状态 */
typedef struct ws_client_s ws_client_t;

/* 重连退避策略 */
typedef enum { RECONNECT_BACKOFF_LINEAR = 0, RECONNECT_BACKOFF_EXPONENTIAL = 1 } reconnect_backoff_t;

/* 全局 WebSocket 配置 */
typedef struct {
	char ws_server_host[256];
	int ws_server_port;
	char ws_server_path[256];
	/* 是否使用 TLS (wss) */
	switch_bool_t ws_use_ssl;
	/* 是否允许自签证书 */
	switch_bool_t ws_allow_selfsigned;
	/* 是否跳过证书主机名检查 (skip hostname check) */
	switch_bool_t ws_skip_cert_hostname_check;
	switch_bool_t ws_enabled;

	/* 重连相关配置 */
	int reconnect_interval;		 /* 重连间隔（秒） */
	int max_retry_count;		 /* 最大重连次数，0=无限，-1=不重连 */
	int send_buffer_size;		 /* 发送缓冲区大小 */
	int connect_timeout;		 /* 连接超时（秒） */
	reconnect_backoff_t backoff; /* 重连退避策略 */
	int max_reconnect_interval;	 /* 最大重连间隔（秒） */
								 // char* iana_name;              /* IANA 编码名称 */

} ws_config_t;

struct ws_client_s {
	struct lws_context *context;
	struct lws *wsi;
	switch_bool_t connected;
	switch_bool_t socketio_ready; /* Socket.IO 已就绪 */
	switch_mutex_t *mutex;
	switch_queue_t *send_queue;
	switch_thread_t *service_thread;
	switch_bool_t running;
	switch_memory_pool_t *pool;

	/* 重连相关状态 */
	ws_config_t *config;		/* 保存配置引用 */
	int retry_count;			/* 当前重连次数 */
	int current_interval;		/* 当前重连间隔 */
	switch_time_t last_attempt; /* 上次尝试时间 */
	switch_bool_t reconnecting; /* 是否正在重连 */
};

typedef struct {

	switch_core_session_t *session;
	const char *uuid;
	switch_media_bug_t *audio_bug;
	switch_media_bug_t *video_bug;

	/* Network settings */
	char dest_ip[256];
	int is_audio;
	int is_video;
	int audio_port;
	int video_port;
	/* Sockets */
	int audio_sock;
	int video_sock;
	struct sockaddr_in audio_addr;
	struct sockaddr_in video_addr;

	/* Flags */
	switch_bool_t running;
	switch_bool_t video_enabled;
	switch_bool_t audio_enabled;
	switch_mutex_t *mutex;
	switch_memory_pool_t *pool;

	char audio_codec[64];		/* 音频编解码器名称 */
	uint32_t audio_sample_rate; /* 音频采样率 */
	uint32_t audio_channels;	/* 音频通道数 */
	uint32_t audio_pt;			/* 音频 Payload Type */

	char video_codec[64];		/* 视频编解码器名称 */
	uint32_t video_sample_rate; /* 视频时钟频率 */
	uint32_t video_pt;			/* 视频 Payload Type */

	/* File recording */
#ifdef _DEBUGFORWARD
	FILE *video_file;
	char video_file_path[512];
#endif

	/* WebSocket 客户端 */
	ws_client_t *ws_client;
} rtpforward_context_t;

/* Global module settings */
struct globals_s {
	switch_memory_pool_t *pool;
	switch_hash_t *hash_table;
	switch_mutex_t *mutex;
	ws_config_t ws_config;
	ws_client_t *ws_client; /* 全局 WebSocket 客户端 */
};

extern struct globals_s globals;

/* 发送队列包结构 */
typedef struct {
	uint8_t *data;
	size_t len;
	switch_bool_t is_video;
} queued_packet_t;

/* 函数声明 */
switch_status_t ws_config_load(ws_config_t *config);
ws_client_t *ws_client_create(ws_config_t *config, switch_memory_pool_t *pool);
void ws_client_destroy(ws_client_t *client);
void ws_client_send_packet(ws_client_t *client, const uint8_t *data, size_t len, switch_bool_t is_video);
void ws_client_send_context_json(ws_client_t *client, rtpforward_context_t *context, const char *event_type);

#endif /* _WS_CLIENT_H_ */
