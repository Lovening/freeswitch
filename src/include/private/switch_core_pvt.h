/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2021, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 * Andrey Volk <andywolk@gmail.com>
 *
 *
 * switch_core.h -- Core Library Private Data (not to be installed into the system)
 * If the last line didn't make sense, stop reading this file, go away!,
 * this file does not exist!!!!
 *
 */
#include "switch_profile.h"

#ifndef WIN32
#include <switch_private.h>
#endif

#ifdef HAVE_MLOCKALL
#include <sys/mman.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#ifndef WIN32
/* setuid, setgid */
#include <unistd.h>

/* getgrnam, getpwnam */
#include <pwd.h>
#include <grp.h>

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#endif

/* #define DEBUG_ALLOC */
#define DO_EVENTS

#define SWITCH_EVENT_QUEUE_LEN 256
#define SWITCH_MESSAGE_QUEUE_LEN 256

#define SWITCH_BUFFER_BLOCK_FRAMES 25
#define SWITCH_BUFFER_START_FRAMES 50

typedef enum {
	SSF_NONE = 0,					   // 无标志
	SSF_DESTROYED = (1 << 0),		   // 会话已销毁
	SSF_WARN_TRANSCODE = (1 << 1),	   // 转码警告
	SSF_HANGUP = (1 << 2),			   // 挂断标志
	SSF_THREAD_STARTED = (1 << 3),	   // 线程已启动
	SSF_THREAD_RUNNING = (1 << 4),	   // 线程正在运行
	SSF_READ_TRANSCODE = (1 << 5),	   // 读取转码
	SSF_WRITE_TRANSCODE = (1 << 6),	   // 写入转码
	SSF_READ_CODEC_RESET = (1 << 7),   // 读取编解码器重置
	SSF_WRITE_CODEC_RESET = (1 << 8),  // 写入编解码器重置
	SSF_DESTROYABLE = (1 << 9),		   // 可销毁标志
	SSF_MEDIA_BUG_TAP_ONLY = (1 << 10) // 媒体bug仅在tap模式下生效
} switch_session_flag_t;

struct switch_core_session {
	switch_memory_pool_t *pool;						 // 会话专用内存池，用于分配会话相关内存
	switch_thread_t *thread;						 // 会话处理线程
	switch_thread_id_t thread_id;					 // 线程ID
	switch_endpoint_interface_t *endpoint_interface; // 端点接口，如SIP、H.323等协议接口
	switch_size_t id;								 // 会话唯一标识ID
	switch_session_flag_t flags;					 // 会话状态标志位
	switch_channel_t *channel;						 // 通道对象，包含呼叫状态和变量

	switch_io_event_hooks_t event_hooks; // IO事件钩子函数集合
	switch_codec_t *read_codec;			 // 读取音频编解码器
	switch_codec_t *real_read_codec;	 // 实际读取编解码器（转码前）
	switch_codec_t *write_codec;		 // 写入音频编解码器
	switch_codec_t *real_write_codec;	 // 实际写入编解码器（转码前）
	switch_codec_t *video_read_codec;	 // 视频读取编解码器
	switch_codec_t *video_write_codec;	 // 视频写入编解码器

	switch_codec_implementation_t read_impl;		// 读取编解码器实现参数
	switch_codec_implementation_t real_read_impl;	// 实际读取编解码器实现参数
	switch_codec_implementation_t write_impl;		// 写入编解码器实现参数
	switch_codec_implementation_t video_read_impl;	// 视频读取编解码器实现参数
	switch_codec_implementation_t video_write_impl; // 视频写入编解码器实现参数

	switch_audio_resampler_t *read_resampler;  // 音频读取重采样器
	switch_audio_resampler_t *write_resampler; // 音频写入重采样器

	switch_mutex_t *mutex;			   // 会话主互斥锁
	switch_mutex_t *stack_count_mutex; // 堆栈计数互斥锁
	switch_mutex_t *resample_mutex;	   // 重采样操作互斥锁
	switch_mutex_t *codec_init_mutex;  // 编解码器初始化互斥锁
	switch_mutex_t *codec_read_mutex;  // 编解码器读取互斥锁
	switch_mutex_t *codec_write_mutex; // 编解码器写入互斥锁
	switch_thread_cond_t *cond;		   // 线程条件变量
	switch_mutex_t *frame_read_mutex;  // 帧读取互斥锁

	switch_thread_rwlock_t *rwlock;	   // 会话读写锁
	switch_thread_rwlock_t *io_rwlock; // IO操作读写锁

	void *streams[SWITCH_MAX_STREAMS]; // 多媒体流数组，支持多路流
	int stream_count;				   // 当前活跃流的数量

	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];	  // 会话UUID字符串表示
	void *private_info[SWITCH_CORE_SESSION_MAX_PRIVATES]; // 私有信息数组，供各模块存储私有数据
	switch_queue_t *event_queue;						  // 事件队列
	switch_queue_t *message_queue;						  // 消息队列
	switch_queue_t *signal_data_queue;					  // 信号数据队列
	switch_queue_t *private_event_queue;				  // 私有事件队列
	switch_queue_t *private_event_queue_pri;			  // 高优先级私有事件队列
	switch_thread_rwlock_t *bug_rwlock;					  // 媒体bug读写锁
	switch_media_bug_t *bugs;							  // 媒体bug链表，用于录音、监听等功能
	switch_app_log_t *app_log;							  // 应用程序日志
	uint32_t stack_count;								  // 堆栈计数器

	switch_buffer_t *raw_write_buffer;					   // 原始写入缓冲区
	switch_frame_t raw_write_frame;						   // 原始写入帧结构
	switch_frame_t enc_write_frame;						   // 编码后写入帧结构
	uint8_t raw_write_buf[SWITCH_RECOMMENDED_BUFFER_SIZE]; // 原始写入数据缓冲区
	uint8_t enc_write_buf[SWITCH_RECOMMENDED_BUFFER_SIZE]; // 编码写入数据缓冲区

	switch_buffer_t *raw_read_buffer;					  // 原始读取缓冲区
	switch_frame_t raw_read_frame;						  // 原始读取帧结构
	switch_frame_t enc_read_frame;						  // 编码后读取帧结构
	uint8_t raw_read_buf[SWITCH_RECOMMENDED_BUFFER_SIZE]; // 原始读取数据缓冲区
	uint8_t enc_read_buf[SWITCH_RECOMMENDED_BUFFER_SIZE]; // 编码读取数据缓冲区

	switch_codec_t bug_codec;			// 媒体bug使用的编解码器
	uint32_t read_frame_count;			// 读取帧计数器
	uint32_t track_duration;			// 跟踪持续时间
	uint32_t track_id;					// 跟踪ID
	switch_log_level_t loglevel;		// 会话日志级别
	uint32_t soft_lock;					// 软锁计数
	switch_ivr_dmachine_t *dmachine[2]; // DTMF检测机器数组
	switch_plc_state_t *plc;			// 丢包补偿状态
	// 核心会话结构体中媒体指针  建立会话与媒体的关联
	switch_media_handle_t *media_handle;						  // 媒体处理句柄
	uint32_t decoder_errors;									  // 解码器错误计数
	switch_core_video_thread_callback_func_t video_read_callback; // 视频读取回调函数
	void *video_read_user_data;									  // 视频读取回调用户数据
	switch_core_video_thread_callback_func_t text_read_callback;  // 文本读取回调函数
	void *text_read_user_data;									  // 文本读取回调用户数据
	switch_io_routines_t *io_override;							  // IO例程覆盖
	switch_slin_data_t *sdata;									  // 签名线性数据

	switch_buffer_t *text_buffer;	   // 文本缓冲区
	switch_buffer_t *text_line_buffer; // 文本行缓冲区
	switch_mutex_t *text_mutex;		   // 文本操作互斥锁
	const char *external_id;		   // 外部系统ID
};

struct switch_media_bug {
	switch_buffer_t *raw_write_buffer;
	switch_buffer_t *raw_read_buffer;
	switch_frame_t *read_replace_frame_in;
	switch_frame_t *read_replace_frame_out;
	switch_frame_t *write_replace_frame_in;
	switch_frame_t *write_replace_frame_out;
	switch_frame_t *native_read_frame;
	switch_frame_t *native_write_frame;
	switch_media_bug_callback_t callback;
	switch_mutex_t *read_mutex;
	switch_mutex_t *write_mutex;
	switch_core_session_t *session;
	void *user_data;
	uint32_t flags;
	uint8_t ready;
	uint8_t data[SWITCH_RECOMMENDED_BUFFER_SIZE];
	int16_t tmp[SWITCH_RECOMMENDED_BUFFER_SIZE];
	time_t stop_time;
	switch_thread_id_t thread_id;
	char *function;
	char *target;
	switch_codec_implementation_t read_impl;
	switch_codec_implementation_t write_impl;
	uint32_t record_frame_size;
	uint32_t record_pre_buffer_count;
	uint32_t record_pre_buffer_max;
	switch_frame_t *ping_frame;
	switch_frame_t *video_ping_frame;
	switch_frame_t *read_demux_frame;
	switch_queue_t *read_video_queue;
	switch_queue_t *write_video_queue;
	switch_queue_t *spy_video_queue[2];
	switch_image_t *spy_img[2];
	switch_vid_spy_fmt_t spy_fmt;
	switch_thread_t *video_bug_thread;

	switch_buffer_t *text_buffer;
	char *text_framedata;
	uint32_t text_framesize;
	switch_mm_t mm;
	struct switch_media_bug *next;
};

typedef enum {
	DBTYPE_DEFAULT = 0,
	DBTYPE_MSSQL = 1,
} switch_dbtype_t;

struct switch_runtime {
	switch_time_t initiated;
	switch_time_t reference;
	int64_t offset;
	switch_event_t *global_vars;
	switch_hash_t *mime_types;
	switch_hash_t *mime_type_exts;
	switch_hash_t *ptimes;
	switch_memory_pool_t *memory_pool;
	const switch_state_handler_table_t *state_handlers[SWITCH_MAX_STATE_HANDLERS];
	int state_handler_index;
	FILE *console;
	uint8_t running;
	char uuid_str[SWITCH_UUID_FORMATTED_LENGTH + 1];
	uint32_t flags;
	switch_time_t timestamp;
	switch_mutex_t *uuid_mutex;
	switch_mutex_t *throttle_mutex;
	switch_mutex_t *session_hash_mutex;
	switch_mutex_t *global_mutex;
	switch_thread_rwlock_t *global_var_rwlock;
	uint32_t sps_total;
	int32_t sps;
	int32_t sps_last;
	int32_t sps_peak;
	int32_t sps_peak_fivemin;
	int32_t sessions_peak;
	int32_t sessions_peak_fivemin;
	switch_log_level_t hard_log_level;
	char *mailer_app;
	char *mailer_app_args;
	uint32_t max_dtmf_duration;
	uint32_t min_dtmf_duration;
	uint32_t default_dtmf_duration;
	switch_frame_t dummy_cng_frame;
	char dummy_data[5];
	switch_bool_t colorize_console;
	char *odbc_dsn;
	char *dbname;
	uint32_t debug_level;
	uint32_t runlevel;
	uint32_t tipping_point;
	uint32_t cpu_idle_smoothing_depth;
	uint32_t microseconds_per_tick;
	int32_t timer_affinity;
	switch_profile_timer_t *profile_timer;
	double profile_time;
	double min_idle_time;
	switch_dbtype_t odbc_dbtype;
	char hostname[256];
	char *switchname;
	int multiple_registrations;
	uint32_t max_db_handles;
	uint32_t db_handle_timeout;
	uint32_t event_heartbeat_interval;
	int cpu_count;
	uint32_t time_sync;
	char *core_db_pre_trans_execute;
	char *core_db_post_trans_execute;
	char *core_db_inner_pre_trans_execute;
	char *core_db_inner_post_trans_execute;
	int events_use_dispatch;
	uint32_t port_alloc_flags;
	char *event_channel_key_separator;
	uint32_t max_audio_channels;
	switch_call_cause_t shutdown_cause;
	uint32_t uuid_version;
};

extern struct switch_runtime runtime;

struct switch_session_manager {
	switch_memory_pool_t *memory_pool;
	switch_hash_t *session_table;
	uint32_t session_count;
	uint32_t session_limit;
	switch_size_t session_id;
	switch_queue_t *thread_queue;
	switch_mutex_t *mutex;
	switch_thread_cond_t *cond; // 条件变量锁
	int running;
	int busy;
};

extern struct switch_session_manager session_manager;

switch_status_t switch_core_sqldb_init(const char **err);
void switch_core_sqldb_destroy(void);
switch_status_t switch_core_sqldb_start(switch_memory_pool_t *pool, switch_bool_t manage);
void switch_core_sqldb_stop(void);
void switch_core_session_init(switch_memory_pool_t *pool);
void switch_core_session_uninit(void);
void switch_core_state_machine_init(switch_memory_pool_t *pool);
switch_memory_pool_t *switch_core_memory_init(void);
void switch_core_memory_stop(void);
