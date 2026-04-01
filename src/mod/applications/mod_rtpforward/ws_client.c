/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * ws_client.c -- WebSocket Client implementation for RTP forwarding
 *
 * Copyright (C) 2025
 */

#include "ws_client.h"

/* WebSocket 协议回调 */
static int ws_callback(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) {
	ws_client_t *client = (ws_client_t *)lws_context_user(lws_get_context(wsi));

	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket connected\n");
		if (client) {
			switch_mutex_lock(client->mutex);
			client->connected = SWITCH_TRUE;
			switch_mutex_unlock(client->mutex);

			/* Socket.IO 握手：发送 "2probe" (Engine.IO ping) */
			lws_callback_on_writable(wsi);
		}
		break;

	case LWS_CALLBACK_CLIENT_RECEIVE:
		/* 处理 Socket.IO 消息 */
		if (in && len > 0) {
			char *msg = (char *)in;
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Socket.IO received: %.*s\n", (int)len, msg);

			/* Socket.IO Engine.IO 握手响应 */
			if (len >= 1) {
				/* '0' = open, '3' = pong, '40' = connect to namespace */
				if (msg[0] == '0') {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Socket.IO Engine.IO session opened\n");
					/* 发送 Socket.IO 连接消息 "40" */
					lws_callback_on_writable(wsi);
				} else if (msg[0] == '3') {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Socket.IO pong received\n");
				} else if (len >= 2 && msg[0] == '4' && msg[1] == '0') {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Socket.IO namespace connected\n");
				}
			}
		}
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE:
		if (client) {
			/* 先处理 Socket.IO 握手 */
			if (!client->socketio_ready) {
				unsigned char buf[LWS_PRE + 128];
				int n;

				/* 发送 Socket.IO 连接消息 "40" (连接到默认命名空间) */
				buf[LWS_PRE] = '4';
				buf[LWS_PRE + 1] = '0';
				n = lws_write(wsi, &buf[LWS_PRE], 2, LWS_WRITE_TEXT);

				if (n >= 0) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Socket.IO handshake sent\n");
					switch_mutex_lock(client->mutex);
					client->socketio_ready = SWITCH_TRUE;
					switch_mutex_unlock(client->mutex);
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Socket.IO handshake failed\n");
				}
				break;
			}

			/* 发送队列中的数据 */
			if (client->send_queue) {
				void *pop = NULL;
				if (switch_queue_trypop(client->send_queue, &pop) == SWITCH_STATUS_SUCCESS && pop) {
					queued_packet_t *pkt = (queued_packet_t *)pop;
					unsigned char buf[LWS_PRE + 65536];

					/* Socket.IO 二进制消息格式: "42" + JSON(["event", data]) */
					/* 我们直接发送二进制数据，使用 "45" (binary event) */
					if (pkt->len > 0 && pkt->len < 65500) {
						/* Socket.IO binary event: "42" + JSON + binary attachments */
						/* 简化版：直接发送二进制数据 */
						memcpy(&buf[LWS_PRE], pkt->data, pkt->len);
						lws_write(wsi, &buf[LWS_PRE], pkt->len, LWS_WRITE_BINARY);
					}

					switch_safe_free(pkt->data);
					switch_safe_free(pkt);

					/* 如果队列还有数据，请求下次写入 */
					if (switch_queue_size(client->send_queue) > 0) { lws_callback_on_writable(wsi); }
				}
			}
		}
		break;

	case LWS_CALLBACK_CLIENT_CLOSED:
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "WebSocket connection closed\n");
		if (client) {
			switch_mutex_lock(client->mutex);
			client->connected = SWITCH_FALSE;
			client->socketio_ready = SWITCH_FALSE;
			switch_mutex_unlock(client->mutex);
		}
		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		/* in 参数包含错误描述字符串 */
		if (in && len > 0) {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WebSocket connection error: %.*s\n", (int)len,
							  (char *)in);
		} else {
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "WebSocket connection error (no details)\n");
		}
		if (client) {
			switch_mutex_lock(client->mutex);
			client->connected = SWITCH_FALSE;
			client->socketio_ready = SWITCH_FALSE;
			switch_mutex_unlock(client->mutex);
		}
		break;

	default:
		break;
	}

	return 0;
}

/* 前向声明 */
static switch_bool_t ws_try_connect(ws_client_t *client);

/* 计算下次重连间隔 */
static int calculate_reconnect_interval(ws_client_t *client) {
	ws_config_t *config = client->config;
	int interval;

	if (config->backoff == RECONNECT_BACKOFF_EXPONENTIAL) {
		/* 指数退避: interval * 2^retry_count */
		interval = config->reconnect_interval * (1 << client->retry_count);
		if (interval > config->max_reconnect_interval) { interval = config->max_reconnect_interval; }
	} else {
		/* 线性: 固定间隔 */
		interval = config->reconnect_interval;
	}

	return interval;
}

/* WebSocket 服务线程 */
static void *SWITCH_THREAD_FUNC ws_service_thread_run(switch_thread_t *thread, void *obj) {
	ws_client_t *client = (ws_client_t *)obj;
	ws_config_t *config = client->config;
	int wait_interval = 0;
	switch_time_t now = 0, elapsed = 0;
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket service thread started\n");

	while (client->running) {
		/* 正常服务 WebSocket 事件 */
		lws_service(client->context, 50);

		/* 检查是否需要重连 */
		if (client->running && !client->connected && !client->reconnecting) {
			/* 检查是否允许重连 */
			if (config->max_retry_count == -1) {
				/* 不重连 */
				continue;
			}

			if (config->max_retry_count > 0 && client->retry_count >= config->max_retry_count) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
								  "WebSocket max retry count (%d) reached, giving up\n", config->max_retry_count);
				continue;
			}

			/* 计算重连间隔 */
			wait_interval = calculate_reconnect_interval(client);
			now = switch_time_now();
			elapsed = (now - client->last_attempt) / 1000000; /* 转换为秒 */

			if (elapsed >= wait_interval) {
				client->reconnecting = SWITCH_TRUE;
				client->retry_count++;

				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
								  "WebSocket reconnecting (attempt %d/%s, interval %ds)...\n", client->retry_count,
								  config->max_retry_count == 0
									  ? "unlimited"
									  : switch_core_sprintf(client->pool, "%d", config->max_retry_count),
								  wait_interval);

				client->last_attempt = now;

				if (ws_try_connect(client)) {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket reconnection initiated\n");
				} else {
					switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
									  "WebSocket reconnection failed to initiate\n");
				}

				client->reconnecting = SWITCH_FALSE;
			}
		} else if (client->connected) {
			/* 连接成功，重置重试计数 */
			if (client->retry_count > 0) {
				switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket connected, resetting retry count\n");
				client->retry_count = 0;
			}
		}
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket service thread stopped\n");

	return NULL;
}

/* 加载配置文件 */
switch_status_t ws_config_load(ws_config_t *config) {
	char *cf = "rtpforward.conf";
	switch_xml_t cfg, xml, settings, param;

	if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		return SWITCH_STATUS_TERM;
	}

	if ((settings = switch_xml_child(cfg, "settings"))) {
		for (param = switch_xml_child(settings, "param"); param; param = param->next) {
			char *var = (char *)switch_xml_attr_soft(param, "name");
			char *val = (char *)switch_xml_attr_soft(param, "value");

			if (!strcasecmp(var, "websocket-enabled")) {
				config->ws_enabled = switch_true(val);
			} else if (!strcasecmp(var, "websocket-ssl")) {
				config->ws_use_ssl = switch_true(val);
			} else if (!strcasecmp(var, "websocket-allow-self-signed") ||
					   !strcasecmp(var, "websocket-allow-selfsigned")) {
				config->ws_allow_selfsigned = switch_true(val);
			} else if (!strcasecmp(var, "websocket-skip-hostname-check") ||
					   !strcasecmp(var, "websocket-skip-cert-hostname-check")) {
				config->ws_skip_cert_hostname_check = switch_true(val);
			} else if (!strcasecmp(var, "websocket-host")) {
				switch_set_string(config->ws_server_host, val);
			} else if (!strcasecmp(var, "websocket-port")) {
				config->ws_server_port = atoi(val);
			} else if (!strcasecmp(var, "websocket-path")) {
				switch_set_string(config->ws_server_path, val);
			} else if (!strcasecmp(var, "reconnect-interval")) {
				config->reconnect_interval = atoi(val);
				if (config->reconnect_interval < 1) config->reconnect_interval = 1;
			} else if (!strcasecmp(var, "max-retry-count")) {
				config->max_retry_count = atoi(val);
			} else if (!strcasecmp(var, "send-buffer-size")) {
				config->send_buffer_size = atoi(val);
				if (config->send_buffer_size < 1024) config->send_buffer_size = 1024;
			} else if (!strcasecmp(var, "connect-timeout")) {
				config->connect_timeout = atoi(val);
				if (config->connect_timeout < 1) config->connect_timeout = 1;
			} else if (!strcasecmp(var, "reconnect-backoff")) {
				if (!strcasecmp(val, "exponential")) {
					config->backoff = RECONNECT_BACKOFF_EXPONENTIAL;
				} else {
					config->backoff = RECONNECT_BACKOFF_LINEAR;
				}
			} else if (!strcasecmp(var, "max-reconnect-interval")) {
				config->max_reconnect_interval = atoi(val);
				if (config->max_reconnect_interval < 1) config->max_reconnect_interval = 60;
			}
		}
	}
	switch_xml_free(xml);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "WebSocket config loaded: %s, Host=%s, Port=%d, Path=%s, "
					  "SSL=%s, Reconnect: interval=%ds, max_retry=%d, backoff=%s, max_interval=%ds\n",
					  config->ws_enabled ? "enabled" : "disabled", config->ws_server_host, config->ws_server_port,
					  config->ws_server_path, config->ws_use_ssl ? "true" : "false", config->reconnect_interval,
					  config->max_retry_count,
					  config->backoff == RECONNECT_BACKOFF_EXPONENTIAL ? "exponential" : "linear",
					  config->max_reconnect_interval);

	return SWITCH_STATUS_SUCCESS;
}

/* 协议定义 - 放在文件级别以便多次使用 */
static const struct lws_protocols ws_protocols[] = {{"", ws_callback, 0, 65536, 0, NULL, 0},
													{NULL, NULL, 0, 0, 0, NULL, 0}};

/* 内部函数：尝试建立 WebSocket 连接 */
static switch_bool_t ws_try_connect(ws_client_t *client) {
	struct lws_client_connect_info ccinfo;
	char originbuf[512] = {0};
	ws_config_t *config = client->config;

	if (!config || !client->context) { return SWITCH_FALSE; }

	memset(&ccinfo, 0, sizeof(ccinfo));
	ccinfo.context = client->context;
	ccinfo.address = config->ws_server_host;
	ccinfo.port = config->ws_server_port;

	if (!config->ws_server_path || !strcmp(config->ws_server_path, "/")) {
		ccinfo.path = "/socket.io/?EIO=4&transport=websocket";
	} else {
		ccinfo.path = config->ws_server_path;
	}

	ccinfo.host = config->ws_server_host;
	if (config->ws_use_ssl) {
		snprintf(originbuf, sizeof(originbuf), "https://%s:%d", config->ws_server_host, config->ws_server_port);
	} else {
		snprintf(originbuf, sizeof(originbuf), "http://%s:%d", config->ws_server_host, config->ws_server_port);
	}
	ccinfo.origin = originbuf;
	ccinfo.protocol = NULL;

	ccinfo.ssl_connection = 0;
	if (config->ws_use_ssl) {
		ccinfo.ssl_connection |= LCCSCF_USE_SSL;
		if (config->ws_allow_selfsigned) { ccinfo.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED; }
		if (config->ws_skip_cert_hostname_check) { ccinfo.ssl_connection |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK; }
	}
	ccinfo.userdata = client;

	client->wsi = lws_client_connect_via_info(&ccinfo);
	if (!client->wsi) {
		switch_log_printf(
			SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "Failed to initiate WebSocket connection to %s://%s:%d%s\n",
			config->ws_use_ssl ? "wss" : "ws", config->ws_server_host, config->ws_server_port, ccinfo.path);
		return SWITCH_FALSE;
	}

	return SWITCH_TRUE;
}

/* 创建 WebSocket 客户端 */
ws_client_t *ws_client_create(ws_config_t *config, switch_memory_pool_t *pool) {
	ws_client_t *client;
	struct lws_context_creation_info info;
	switch_threadattr_t *thd_attr;

	if (!config->ws_enabled || !config->ws_server_host[0]) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "WebSocket disabled or host not configured\n");
		return NULL;
	}

	client = switch_core_alloc(pool, sizeof(ws_client_t));
	memset(client, 0, sizeof(ws_client_t));
	client->pool = pool;
	client->config = config; /* 保存配置引用 */
	client->socketio_ready = SWITCH_FALSE;
	client->retry_count = 0;
	client->reconnecting = SWITCH_FALSE;
	client->last_attempt = switch_time_now();

	switch_mutex_init(&client->mutex, SWITCH_MUTEX_NESTED, pool);
	switch_queue_create(&client->send_queue, 1000, pool);

	/* 初始化 libwebsockets context */
	lws_set_log_level(LLL_ERR | LLL_WARN | LLL_NOTICE | LLL_INFO, NULL);

	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = ws_protocols;
	info.gid = -1;
	info.uid = -1;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.user = client;

	client->context = lws_create_context(&info);
	if (!client->context) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to create WebSocket context\n");
		return NULL;
	}

	/* 尝试首次连接 */
	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Connecting to %s://%s:%d%s\n",
					  config->ws_use_ssl ? "wss" : "ws", config->ws_server_host, config->ws_server_port,
					  config->ws_server_path);

	if (!ws_try_connect(client)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "Initial WebSocket connection failed, will retry in background\n");
		/* 不返回 NULL，让服务线程处理重连 */
	}

	/* 启动服务线程 */
	client->running = SWITCH_TRUE;
	switch_threadattr_create(&thd_attr, pool);
	switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
	switch_thread_create(&client->service_thread, thd_attr, ws_service_thread_run, client, pool);

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
					  "WebSocket client created (reconnect: interval=%ds, max_retry=%d, backoff=%s)\n",
					  config->reconnect_interval, config->max_retry_count,
					  config->backoff == RECONNECT_BACKOFF_EXPONENTIAL ? "exponential" : "linear");

	return client;
}

/* 销毁 WebSocket 客户端 */
void ws_client_destroy(ws_client_t *client) {
	void *pop;

	if (!client) { return; }

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "Destroying WebSocket client\n");

	client->running = SWITCH_FALSE;

	if (client->service_thread) {
		switch_status_t st;
		switch_thread_join(&st, client->service_thread);
	}

	/* 清空队列 */
	while (switch_queue_trypop(client->send_queue, &pop) == SWITCH_STATUS_SUCCESS) {
		if (pop) {
			queued_packet_t *pkt = (queued_packet_t *)pop;
			switch_safe_free(pkt->data);
			switch_safe_free(pkt);
		}
	}

	if (client->context) { lws_context_destroy(client->context); }

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "WebSocket client destroyed\n");
}

/* 通过 WebSocket 发送数据包 */
void ws_client_send_packet(ws_client_t *client, const uint8_t *data, size_t len, switch_bool_t is_video) {
	queued_packet_t *pkt;
	switch_bool_t is_ready = SWITCH_FALSE;

	if (!client || !data || len == 0) { return; }

	/* 检查连接状态（需要加锁保护） */
	switch_mutex_lock(client->mutex);
	is_ready = (client->connected && client->socketio_ready);
	switch_mutex_unlock(client->mutex);

	if (!is_ready) { return; }

	pkt = malloc(sizeof(queued_packet_t));
	if (!pkt) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to allocate packet structure\n");
		return;
	}

	pkt->data = malloc(len);
	if (!pkt->data) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Failed to allocate packet data\n");
		switch_safe_free(pkt);
		return;
	}

	memcpy(pkt->data, data, len);
	pkt->len = len;
	pkt->is_video = is_video;

	if (switch_queue_trypush(client->send_queue, pkt) != SWITCH_STATUS_SUCCESS) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING, "WebSocket send queue full, dropping packet\n");
		switch_safe_free(pkt->data);
		switch_safe_free(pkt);
	} else {
		/* 通知可写 */
		lws_callback_on_writable(client->wsi);
	}
}

/* 发送 rtpforward_context_t 的 JSON 格式数据到 WebSocket 服务器 */
void ws_client_send_context_json(ws_client_t *client, rtpforward_context_t *context, const char *event_type) {

	char json_buffer[2048];
	int offset = 0;
	double timestamp;

	if (!client || !context || !event_type) { return; }

	/* 检查是否已连接 */
	switch_mutex_lock(client->mutex);
	if (!client->connected) {
		switch_mutex_unlock(client->mutex);
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
						  "WebSocket not connected, cannot send context JSON\n");
		return;
	}
	switch_mutex_unlock(client->mutex);

	/* 手动构建 JSON 字符串，避免 cJSON 的内存分配开销 */
	timestamp = (double)switch_time_now() / 1000000.0;

	/* 开始构建 JSON */
	offset += switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset,
							  "{\"event\":\"%s\",\"timestamp\":%.3f", event_type, timestamp);

	/* 添加 UUID */
	if (context->uuid) {
		offset +=
			switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset, ",\"uuid\":\"%s\"", context->uuid);
	}

	/* 添加端口信息 */
	offset += switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset,
							  ",\"audio_port\":%d,\"video_port\":%d", context->audio_port, context->video_port);

	/* 添加状态标志 */
	offset += switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset,
							  ",\"running\":%s,\"audio_enabled\":%s,\"video_enabled\":%s",
							  context->running ? "true" : "false", context->is_audio ? "true" : "false",
							  context->is_video ? "true" : "false");

	/* 添加音频编解码器信息 */
	offset +=
		switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset,
						",\"audio\":{\"codec\":\"%s\",\"sample_rate\":%u,\"channels\":%u,\"payload_type\":%u}",
						context->audio_codec, context->audio_sample_rate, context->audio_channels, context->audio_pt);

	/* 添加视频编解码器信息（如果启用） */
	if (context->video_enabled) {
		offset += switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset,
								  ",\"video\":{\"codec\":\"%s\",\"sample_rate\":%u,\"payload_type\":%u}",
								  context->video_codec, context->video_sample_rate, context->video_pt);
	}

	/* 结束 JSON 对象 */
	offset += switch_snprintf(json_buffer + offset, sizeof(json_buffer) - offset, "}");

	/* 检查缓冲区溢出 */
	if (offset >= sizeof(json_buffer) - 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "JSON buffer overflow, message truncated\n");
		return;
	}

	switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Sending context JSON (%d bytes): %s\n", offset,
					  json_buffer);

	//  {
	//   "event": "rtpforward_started",
	//   "timestamp": 1732627200.5,
	//   "uuid": "abc123-session-uuid",
	//   "dest_ip": "127.0.0.1",
	//   "audio_port": 5000,
	//   "video_port": 5002,
	//   "running": true,
	//   "audio_enabled": true,
	//   "video_enabled": true,
	//   "audio": {
	//     "codec": "PCMU",
	//     "sample_rate": 8000,
	//     "channels": 1,
	//     "payload_type": 0
	//   },
	//   "video": {
	//     "codec": "H264",
	//     "sample_rate": 90000,
	//     "payload_type": 96
	//   }
	// }

	/* 通过 WebSocket 发送 JSON（作为文本消息，不是 RTP 包） */
	ws_client_send_packet(client, (const uint8_t *)json_buffer, offset, SWITCH_FALSE);
}
