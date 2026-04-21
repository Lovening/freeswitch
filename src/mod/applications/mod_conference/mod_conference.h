/*
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2014, Anthony Minessale II <anthm@freeswitch.org>
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
 * Neal Horman <neal at wanlink dot com>
 * Bret McDanel <trixter at 0xdecafbad dot com>
 * Dale Thatcher <freeswitch at dalethatcher dot com>
 * Chris Danielson <chris at maxpowersoft dot com>
 * Rupa Schomaker <rupa@rupa.com>
 * David Weekly <david@weekly.org>
 * Joao Mesquita <jmesquita@gmail.com>
 * Raymond Chandler <intralanman@freeswitch.org>
 * Seven Du <dujinfang@gmail.com>
 * Emmanuel Schmidbauer <e.schmidbauer@gmail.com>
 * William King <william.king@quentustech.com>
 *
 * mod_conference.c -- Software Conference Bridge
 *
 */

#ifndef MOD_CONFERENCE_H
#define MOD_CONFERENCE_H

#include <switch.h>

/* FFmpeg filter support for video overlay */
#ifdef HAVE_LIBAVFILTER
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavfilter/buffersink.h>
#include <libavutil/opt.h>
#include <libavutil/frame.h>
#include <libavutil/imgutils.h>
#endif

/* DEFINES */

#ifdef OPENAL_POSITIONING
#define AL_ALEXT_PROTOTYPES
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>
#endif

#define DEFAULT_LAYER_TIMEOUT 10
#define DEFAULT_AGC_LEVEL 1100
#define CONFERENCE_UUID_VARIABLE "conference_uuid"

/* Size to allocate for audio buffers */
#define CONF_BUFFER_SIZE 1024 * 128
#define CONF_EVENT_MAINT "conference::maintenance"
#define CONF_EVENT_CDR "conference::cdr"
#define CONF_DEFAULT_LEADIN 20

#define CONF_DBLOCK_SIZE CONF_BUFFER_SIZE
#define CONF_DBUFFER_SIZE CONF_BUFFER_SIZE
#define CONF_DBUFFER_MAX 0
#define CONF_CHAT_PROTO "conf"

#ifndef MIN
#define MIN(a, b) ((a)<(b)?(a):(b))
#endif

/* the rate at which the infinite impulse response filter on speaker score will decay. */
#define SCORE_DECAY 0.8
/* the maximum value for the IIR score [keeps loud & longwinded people from getting overweighted] */
#define SCORE_MAX_IIR 25000
/* the minimum score for which you can be considered to be loud enough to now have the floor */
#define SCORE_IIR_SPEAKING_MAX 300
/* the threshold below which you cede the floor to someone loud (see above value). */
#define SCORE_IIR_SPEAKING_MIN 100
/* the FPS of the conference canvas */
#define FPS 30
/* max supported layers in one mcu */
#define MCU_MAX_LAYERS 64

/* video layout scale factor */
#define VIDEO_LAYOUT_SCALE 360.0f

#define CONFERENCE_MUX_DEFAULT_LAYOUT "group:grid"
#define CONFERENCE_MUX_DEFAULT_SUPER_LAYOUT "grid"
#define CONFERENCE_CANVAS_DEFAULT_WIDTH 1280
#define CONFERENCE_CANVAS_DEFAULT_HIGHT 720
#define MAX_CANVASES 20
#define SUPER_CANVAS_ID MAX_CANVASES
#define test_eflag(conference, flag) ((conference)->eflags & flag)

#define lock_member(_member) switch_mutex_lock(_member->write_mutex); switch_mutex_lock(_member->read_mutex)
#define unlock_member(_member) switch_mutex_unlock(_member->read_mutex); switch_mutex_unlock(_member->write_mutex)

//#define lock_member(_member) switch_mutex_lock(_member->write_mutex)
//#define unlock_member(_member) switch_mutex_unlock(_member->write_mutex)

#define CONFFUNCAPISIZE (sizeof(conference_api_sub_commands)/sizeof(conference_api_sub_commands[0]))

#define MAX_MUX_CODECS 50

#define ALC_HRTF_SOFT  0x1992

#define validate_pin(buf, pin, mpin)									\
	pin_valid = (!zstr(pin) && strcmp(buf, pin) == 0);					\
	if (!pin_valid && !zstr(mpin) && strcmp(buf, mpin) == 0) {			\
		switch_log_printf(SWITCH_CHANNEL_SESSION_LOG(session), SWITCH_LOG_DEBUG, "Moderator PIN found!\n"); \
		pin_valid = 1;													\
		mpin_matched = 1;												\
	}

/* STRUCTS */

typedef enum {
	CONF_SILENT_REQ = (1 << 0),
	CONF_SILENT_DONE = (1 << 1)
} conference_app_flag_t;

extern char *mod_conference_cf_name;
extern char *api_syntax;
extern int EC;

typedef enum {
	FILE_STOP_CURRENT,
	FILE_STOP_ALL,
	FILE_STOP_ASYNC
} file_stop_t;

/* Global Values */
typedef struct conference_globals_s {
	switch_memory_pool_t *conference_pool;
	switch_mutex_t *conference_mutex;
	switch_hash_t *conference_hash;
	switch_mutex_t *id_mutex;
	switch_mutex_t *hash_mutex;
	switch_mutex_t *setup_mutex;
	uint32_t id_pool;
	int32_t running;
	uint32_t threads;
	switch_event_channel_id_t event_channel_id;
} conference_globals_t;

extern conference_globals_t conference_globals;

/* forward declaration for conference_obj and caller_control */
struct conference_member;
typedef struct conference_member conference_member_t;

struct caller_control_actions;

typedef struct caller_control_actions {
	char *binded_dtmf;
	char *data;
	char *expanded_data;
} caller_control_action_t;

typedef struct caller_control_menu_info {
	switch_ivr_menu_t *stack;
	char *name;
} caller_control_menu_info_t;

typedef enum {
	MFLAG_RUNNING,  // 成员的会议线程是否在运行。成员加入时置 1，离开/踢出时清 0，是整个成员事件循环的生命周期标志
	MFLAG_CAN_SPEAK, // 成员是否可以说话（发送音频）。被 mute 时清 0，unmute 时置 1
	MFLAG_CAN_HEAR, // 成员是否可以听到其他人（接收音频）。被 deaf 时清 0，undeaf 时置 1
	MFLAG_KICKED, // 成员已被踢出会议。设置后成员的循环会检测到并退出
	MFLAG_ITHREAD, //  标记输入线程已启动。用于防止重复启动输入线程，输入线程退出时清除
	MFLAG_NOCHANNEL, //  成员没有关联的通道（channel-less member）。用于虚拟成员（如录音、文件播放等不绑定 session 的成员）
	MFLAG_INTREE, //  成员已在会议成员树中（已加入成员链表）。用于判断成员是否处于活跃状态
	MFLAG_NO_MINIMIZE_ENCODING, //禁用最小化编码优化。每个成员使用独立编码器而非共享编码，由 video_use_dedicated_encoder 通道变量设置
	MFLAG_FLUSH_BUFFER, // 刷新音频缓冲区标志。用于音频重置时清空积压数据
	MFLAG_ENDCONF, // 此成员离开时自动结束整个会议。通常用于会议创建者或主持人
	MFLAG_MANDATORY_MEMBER_ENDCONF, // 此成员是"强制结束会议成员"。当所有带此标志的成员都离开后，会议才结束
	MFLAG_HAS_AUDIO, // 成员当前有音频数据到来。在音频处理循环中动态设置/清除
	MFLAG_TALKING, // 成员正在说话（检测到能量超过阈值）。用于讲话者事件通知和视频 floor 切换
	MFLAG_RESTART, //代码中已定义但未使用的死枚举值
	MFLAG_MINTWO, // 至少两个人在此成员发言时才能听到（min-two 模式）
	MFLAG_MUTE_DETECT, // 启用静音检测。当成员被 mute 但试图说话时，触发事件通知
	MFLAG_DIST_DTMF, // 将 DTMF 透传/分发给其他成员。默认会议会消费 DTMF 用于控制，此标志让 DTMF 继续传递
	MFLAG_MOD, // MFLAG_MOD
	MFLAG_INDICATE_MUTE, // 一次性标志：触发播放 mute 提示音（如"您已被静音"），播放后自动清除
	MFLAG_INDICATE_UNMUTE, //一次性标志：触发播放 unmute 提示音
	MFLAG_INDICATE_BLIND, //一次性标志：触发播放 deaf（视频盲）提示音，播放 deaf_sound
	MFLAG_INDICATE_UNBLIND, // 一次性标志：触发播放 undeaf 提示音
	MFLAG_NOMOH, //不播放等待音乐（Music On Hold），即使会议只有一个人
	MFLAG_VIDEO_BRIDGE, //成员使用视频桥接模式（直接转发而非 MCU 混合），跳过画布渲染
	MFLAG_INDICATE_MUTE_DETECT, //一次性标志：触发播放"您正在静音状态下试图说话"的提示音
	MFLAG_PAUSE_RECORDING, // 暂停此成员的录音
	MFLAG_ACK_VIDEO, // 已确认/接收过视频。首次收到视频帧时设置，用于从头像切换到真实视频
	MFLAG_GHOST, // 幽灵成员：存在于会议中但不被其他成员感知，常用于监听/录音
	MFLAG_JOIN_ONLY, // 只加入模式，不启动音频/视频处理线程
	MFLAG_POSITIONAL, // 启用位置音频（3D 空间音频），根据成员在虚拟空间的位置调整音量和左右声道
	MFLAG_NO_POSITIONAL, // 显式禁用位置音频，即使会议配置了位置音频
	MFLAG_JOIN_VID_FLOOR, // 加入会议后自动抢占视频 floor（成为视频主讲人）
	MFLAG_RECEIVING_VIDEO, //成员正在接收视频流。用于视频流状态跟踪
	MFLAG_CAN_SEE, // 成员是否可以看到其他人的视频。类似音频的 deaf，这是视频的"接收"控制
	MFLAG_CAN_BE_SEEN, // 成员是否可以被其他人看到（发送视频）。vmute 时清 0，unvmute 时置 1。我们之前分析过
	MFLAG_SECOND_SCREEN, // 第二屏幕模式：只看视频不参与音频互动。设置时自动清除 CAN_SPEAK、CAN_HEAR、CAN_BE_SEEN
	MFLAG_SILENT, // 成员静默加入/离开，不播放进入/退出提示音
	MFLAG_FLIP_VIDEO, //水平翻转视频画面
	MFLAG_ROTATE_VIDEO, // 旋转视频画面
	MFLAG_MIRROR_VIDEO, // 镜像视频画面（通常用于自拍视角）
	MFLAG_INDICATE_DEAF, // 一次性标志：触发播放 deaf 提示音（音频层面）
	MFLAG_INDICATE_UNDEAF, // 一次性标志：触发播放 undeaf 提示音
	MFLAG_TALK_DATA_EVENTS,
	MFLAG_NO_VIDEO_BLANKS, //不发送视频空白帧（blank 帧），保持最后一帧
	MFLAG_VIDEO_JOIN, //标记已完成视频加入流程
	MFLAG_DED_VID_LAYER, //使用专属视频层（dedicated video layer），不参与自动层分配和 floor 切换，始终占据一个固定的画面位置
	MFLAG_HOLD, //成员处于挂起状态（hold），既不发送也不接收音视频
	MFLAG_SKIP_DTMF, // 完全跳过/忽略 DTMF 处理。DTMF 既不触发会议控制，也不传递
	///////////////////////////
	MFLAG_MAX
} member_flag_t;

typedef enum {
	CFLAG_RUNNING, //运行中 会议正在运行的核心标志
	CFLAG_DYNAMIC, //动态会议 临时创建的会议（非配置文件预定义）
	CFLAG_ENFORCE_MIN,
	CFLAG_DESTRUCT, // 销毁标志
	CFLAG_LOCKED, //锁定状态
	CFLAG_ANSWERED,
	CFLAG_BRIDGE_TO,
	CFLAG_WAIT_MOD,
	CFLAG_VID_FLOOR, //视频发言权
	CFLAG_WASTE_FLAG,
	CFLAG_OUTCALL,
	CFLAG_INHASH,
	CFLAG_EXIT_SOUND,
	CFLAG_ENTER_SOUND,
	CFLAG_USE_ME,
	CFLAG_AUDIO_ALWAYS,
	CFLAG_ENDCONF_FORCED,
	CFLAG_RFC4579,
	CFLAG_FLOOR_CHANGE,
	CFLAG_VID_FLOOR_LOCK,
	CFLAG_JSON_EVENTS,
	CFLAG_LIVEARRAY_SYNC,
	CFLAG_CONF_RESTART_AUTO_RECORD,
	CFLAG_POSITIONAL,
	CFLAG_TRANSCODE_VIDEO,
	CFLAG_VIDEO_MUXING,
	CFLAG_MINIMIZE_VIDEO_ENCODING,
	CFLAG_MANAGE_INBOUND_VIDEO_BITRATE,
	CFLAG_JSON_STATUS,
	CFLAG_VIDEO_BRIDGE_FIRST_TWO,
	CFLAG_VIDEO_REQUIRED_FOR_CANVAS,
	CFLAG_PERSONAL_CANVAS,
	CFLAG_REFRESH_LAYOUT,
	CFLAG_VIDEO_MUTE_EXIT_CANVAS,
	CFLAG_NO_MOH,
	CFLAG_DED_VID_LAYER_AUDIO_FLOOR,
	CFLAG_BREAKABLE,
	/////////////////////////////////
	CFLAG_MAX
} conference_flag_t;

typedef struct conference_cdr_node_s {
	switch_caller_profile_t *cp;
	char *record_path;
	switch_time_t join_time;
	switch_time_t leave_time;
	member_flag_t mflags[MFLAG_MAX];
	uint32_t id;
	conference_member_t *member;
	switch_event_t *var_event;
	struct conference_cdr_node_s *next;
} conference_cdr_node_t;

typedef enum {
	CDRR_LOCKED = 1,
	CDRR_PIN,
	CDRR_MAXMEMBERS
} cdr_reject_reason_t;

typedef struct conference_cdr_reject_s {
	switch_caller_profile_t *cp;
	switch_time_t reject_time;
	cdr_reject_reason_t reason;
	struct conference_cdr_reject_s *next;
} conference_cdr_reject_t;

typedef enum {
	CDRE_NONE,
	CDRE_AS_CONTENT,
	CDRE_AS_FILE
} cdr_event_mode_t;


struct call_list {
	char *string;
	int iteration;
	struct call_list *next;
};
typedef struct call_list call_list_t;



typedef enum {
	RFLAG_CAN_SPEAK = (1 << 0),
	RFLAG_CAN_HEAR = (1 << 1),
	RFLAG_CAN_SEND_VIDEO = (1 << 2)
} relation_flag_t;

typedef enum {
	NODE_TYPE_FILE,
	NODE_TYPE_SPEECH
} node_type_t;

typedef enum {
	NFLAG_NONE = (1 << 0),
	NFLAG_PAUSE = (1 << 1)
} node_flag_t;

typedef enum {
	EFLAG_HOLD_MEMBER = (1 << 0),
	EFLAG_DEL_MEMBER = (1 << 1),
	EFLAG_ENERGY_LEVEL = (1 << 2),
	EFLAG_VOLUME_LEVEL = (1 << 3),
	EFLAG_GAIN_LEVEL = (1 << 4),
	EFLAG_DTMF = (1 << 5),
	EFLAG_STOP_TALKING = (1 << 6),
	EFLAG_START_TALKING = (1 << 7),
	EFLAG_MUTE_MEMBER = (1 << 8),
	EFLAG_BLIND_MEMBER = (1 << 9),
	EFLAG_DEAF_MEMBER = (1 << 10),
	EFLAG_UNUSED1 = (1 << 11),
	EFLAG_KICK_MEMBER = (1 << 12),
	EFLAG_DTMF_MEMBER = (1 << 13),
	EFLAG_ENERGY_LEVEL_MEMBER = (1 << 14),
	EFLAG_VOLUME_IN_MEMBER = (1 << 15),
	EFLAG_VOLUME_OUT_MEMBER = (1 << 16),
	EFLAG_PLAY_FILE = (1 << 17),
	EFLAG_PLAY_FILE_MEMBER = (1 << 18),
	EFLAG_SPEAK_TEXT = (1 << 19),
	EFLAG_SPEAK_TEXT_MEMBER = (1 << 20),
	EFLAG_LOCK = (1 << 21),
	EFLAG_UNLOCK = (1 << 22),
	EFLAG_TRANSFER = (1 << 23),
	EFLAG_BGDIAL_RESULT = (1 << 24),
	EFLAG_FLOOR_CHANGE = (1 << 25),
	EFLAG_MUTE_DETECT = (1 << 26),
	EFLAG_RECORD = (1 << 27),
	EFLAG_HUP_MEMBER = (1 << 28),
	EFLAG_PLAY_FILE_DONE = (1 << 29),
	EFLAG_SET_POSITION_MEMBER = (1 << 30)
} event_type_t;

#ifdef OPENAL_POSITIONING
typedef struct al_handle_s {
	switch_mutex_t *mutex;
	ALCdevice *device;
	ALCcontext *context;
	ALuint source;
	ALuint buffer_in[2];
	int setpos;
	ALfloat pos_x;
	ALfloat pos_y;
	ALfloat pos_z;
} al_handle_t;

void conference_al_close(al_handle_t *al);
#else
typedef struct al_handle_s {
	int unsupported;
	switch_mutex_t *mutex;
} al_handle_t;
#endif
struct conference_obj;

typedef struct conference_file_node {
	switch_file_handle_t fh;
	switch_speech_handle_t *sh;
	node_flag_t flags;
	node_type_t type;
	uint8_t done;
	uint8_t async;
	switch_memory_pool_t *pool;
	uint32_t leadin;
	struct conference_file_node *next;
	char *file;
	switch_bool_t mux;
	uint32_t member_id;
	al_handle_t *al;
	int layer_id;
	int canvas_id;
	struct conference_obj *conference;
	char *res_id;
	int loops;
	int new_fnode;
	int layer_lock;
	switch_core_video_filter_t filters;
} conference_file_node_t;

typedef enum {
	REC_ACTION_STOP = 1,
	REC_ACTION_PAUSE,
	REC_ACTION_RESUME
} recording_action_type_t;

/* conference xml config sections */
typedef struct conference_xml_cfg {
	switch_xml_t profile;
	switch_xml_t controls;
} conference_xml_cfg_t;

struct vid_helper {
	conference_member_t *member_a;
	conference_member_t *member_b;
	int up;
};


typedef struct mcu_layer_geometry_s {
	int x;
	int y;
	int scale;
	int hscale;
	int floor;
	int flooronly;
	int fileonly;
	int overlap;
	int zoom;
	int border;
	char *res_id;
	char *role_id;
	char *audio_position;
} mcu_layer_geometry_t;

typedef struct mcu_layer_def_s {
	char *name;
	mcu_layer_geometry_t layers[MCU_MAX_LAYERS];
} mcu_layer_def_t;


typedef struct mcu_layer_cam_opts_s {
	int manual_pan;
	int manual_zoom;
	int autozoom;
	int autopan;
	int zoom_factor;
	int snap_factor;
	int zoom_move_factor;
	int pan_speed;
	int pan_accel_speed;
	int pan_accel_min;
	int zoom_speed;
	int zoom_accel_speed;
	int zoom_accel_min;
} mcu_layer_cam_opts_t;

struct mcu_canvas_s;

typedef struct mcu_layer_s {
	/* ==================== 几何与成员信息 ==================== */
	mcu_layer_geometry_t geometry;     /* 层的几何配置（位置、尺寸等布局参数） */
	int member_id;                     /* 关联的会议成员ID（0表示未使用） */
	int idx;                           /* 层在画布中的索引位置 */
	int tagged;                        /* 标志：层是否被标记（用于布局管理） */
	int bugged;                        /* 标志：是否启用水印叠加 */

	/* ==================== 屏幕与位置参数 ==================== */
	uint32_t screen_w;                /* 屏幕宽度（像素）- 分配给此层的显示区域宽度 */
	uint32_t screen_h;                /* 屏幕高度（像素）- 分配给此层的显示区域高度 */
	int x_pos;                        /* 层在画布上的X坐标（相对于画布原点） */
	int y_pos;                        /* 层在画布上的Y坐标（相对于画布原点） */

	/* ==================== 叠加状态标志（防止重复叠加） ==================== */
	int banner_patched;               /* 标志：横幅文字是否已叠加（避免每帧重复渲染） */
	int mute_patched;                 /* 标志：静音图标是否已叠加 */
	int avatar_patched;               /* 标志：头像图片是否已叠加 */

	/* ==================== 刷新与状态标志 ==================== */
	int refresh;                      /* 标志：层是否需要刷新（重绘） */
	int clear;                        /* 标志：层是否需要清除 */
	int is_avatar;                    /* 标志：当前是否显示为头像模式（无视频时） */

	/* ==================== 视频裁剪参数 ==================== */
	int crop_x;                       /* 裁剪区域的X起始坐标 */
	int crop_y;                       /* 裁剪区域的Y起始坐标 */
	int crop_w;                       /* 裁剪区域的宽度 */
	int crop_h;                       /* 裁剪区域的高度 */

	/* ==================== 缓存与计数 ==================== */
	int last_w;                       /* 上一次渲染的宽度（用于检测尺寸变化） */
	int last_h;                       /* 上一次渲染的高度（用于检测尺寸变化） */
	uint32_t img_count;               /* 接收到的图像帧计数器 */

	/* ==================== 图像资源指针 ==================== */
	switch_image_t *img;              /* 主要图像指针（缩放后的视频帧） */
	switch_image_t *cur_img;          /* 当前原始图像指针（未缩放的输入帧） */
	switch_image_t *overlay_img;      /* 叠加层图像（如半透明遮罩、边框等） */
	switch_image_t *banner_img;       /* 横幅文字图像（包含文字的透明PNG） */
	switch_image_t *logo_img;         /* Logo图标图像（会议成员头像/图标） */
	switch_image_t *mute_img;         /* 静音图标图像（显示静音状态） */

	/* ==================== 文字与文件处理 ==================== */
	switch_img_txt_handle_t *txthandle; /* 文字渲染句柄（用于绘制横幅文字） */
	conference_file_node_t *fnode;      /* 正在播放的文件节点（如等待音乐） */

	/* ==================== Logo配置 ==================== */
	switch_img_position_t logo_pos;    /* Logo位置配置（如左上角、右下角等） */
	switch_img_fit_t logo_fit;         /* Logo缩放适配模式（FIT_SIZE/FIT_SCALE等） */

	/* ==================== 画布与成员关联 ==================== */
	struct mcu_canvas_s *canvas;       /* 所属的画布对象指针（父容器） */
	int need_patch;                   /* 标志：层是否需要补丁/修补到画布 */
	conference_member_t *member;      /* 关联的会议成员对象指针 */

	/* ==================== 水印相关 ==================== */
	switch_frame_t bug_frame;          /* 水印帧数据（如电视台Logo、公司标识） */

	/* ==================== 几何变换配置 ==================== */
	switch_frame_geometry_t last_geometry;      /* 上一次的几何参数（用于检测变化） */
	switch_frame_geometry_t auto_geometry;      /* 自动调整的几何参数（AI取景等） */
	switch_frame_geometry_t zoom_geometry;      /* 缩放几何参数（数字放大） */
	switch_frame_geometry_t pan_geometry;       /* 平移几何参数（数字平移） */
	switch_frame_geometry_t manual_geometry;    /* 手动设置的几何参数（覆盖自动） */

	/* ==================== 摄像头选项 ==================== */
	mcu_layer_cam_opts_t cam_opts;    /* 摄像头控制选项（自动缩放、自动平移等参数） */

	/* ==================== 叠加层同步与滤镜 ==================== */
	switch_mutex_t *overlay_mutex;    /* 叠加层操作互斥锁（保护overlay_img的并发访问） */
	switch_core_video_filter_t overlay_filters; /* 叠加层滤镜效果（灰度、棕褐色等） */

	/* ==================== 边框 ==================== */
	int manual_border;                /* 手动设置的边框宽度（像素） */

	/* ==================== FFmpeg Filter 支持（用户ID/名称叠加） ==================== */
#ifdef HAVE_LIBAVFILTER
	AVFilterGraph *filter_graph;      /* FFmpeg filter graph（滤镜处理图） */
	AVFilterContext *buffersrc_ctx;   /* 输入源上下文（接收原始视频帧） */
	AVFilterContext *buffersink_ctx;  /* 输出源上下文（获取处理后的帧） */
	AVFrame *filter_frame_in;         /* 输入 AVFrame */
	AVFrame *filter_frame_out;        /* 输出 AVFrame */
	int filter_w;                     /* Filter graph input width */
	int filter_h;                     /* Filter graph input height */
	switch_bool_t filter_enabled;     /* 标志：filter是否启用 */
	switch_bool_t filter_last_can_speak; /* 上一帧的静音状态，用于检测变化 */
	int64_t filter_pts;               /* 当前送入 filter 的 pts */
	int64_t filter_pts_step;          /* 每帧 pts 增量，单位是 1/90000 */
	int filter_net_frame_count;       /* 网络状态采样帧计数器（每30帧采样一次） */
	char filter_net_text[64];         /* 上次用于构建 filter 的网络状态文本 */
#endif
} mcu_layer_t;

typedef struct video_layout_s {
	char *name;
	char *audio_position;
	char *bgimg;
	char *fgimg;
	char *transition_in;
	char *transition_out;
	mcu_layer_geometry_t images[MCU_MAX_LAYERS];
	int layers;
} video_layout_t;

typedef struct video_layout_node_s {
	video_layout_t *vlayout;
	struct video_layout_node_s *next;
} video_layout_node_t;

typedef struct layout_group_s {
	video_layout_node_t *layouts;
} layout_group_t;

typedef struct codec_set_s {
	switch_codec_t codec;  // 编码器实例
	switch_frame_t frame; // 编码输出帧
	uint8_t *packet;  // RTP 包缓冲
	switch_image_t *scaled_img;  // 缩放中间图像（用于非标分辨率编码）
	uint8_t fps_divisor; // 帧率除数（降低编码帧率）
	uint32_t frame_count;  // 帧计数
	char *video_codec_group;  // 编码器组名
} codec_set_t;


typedef struct mcu_canvas_s {
	int width; // 画布宽度（像素），默认 1280
	int height; // 画布高度（像素），默认 720
	switch_image_t *img; //  画布主图像（I420/YUV420P 格式）
	mcu_layer_t layers[MCU_MAX_LAYERS]; //图层数组，最多 64 个槽位
	int res_count; // 使用 reservation_id 预留的层数
	int role_count;  // 使用 role_id 角色绑定的层数
	int total_layers;  // 当前布局定义的总层数
	int layers_used; // 当前已被成员占用的层数
	int layout_floor_id; // 发言权(floor)对应的图层索引，-1 表示无
	int refresh; // 标志：画布需要刷新（重绘）
	int send_keyframe;  // 关键帧请求倒计数（每10帧触发一次）
	int play_file; // 文件播放状态机: 0=无, 1=开始播放, -1=播放中
	int video_count;   // 当前有视频的成员数
	char *video_layout_group; // 当前使用的布局组名（如 "grid"）
	switch_rgb_color_t bgcolor; // 画布背景色（无视频区域）
	switch_rgb_color_t border_color;  // 图层边框颜色
	switch_rgb_color_t letterbox_bgcolor;  // 宽高比不匹配时的黑边颜色
	switch_mutex_t *mutex;  // 画布主锁（保护图层操作）
	switch_mutex_t *write_mutex; // 写入锁（保护编码器写入）
	switch_timer_t timer;  // 帧率定时器（默认 soft 33ms/30fps）
	switch_memory_pool_t *pool;   // 内存池（从 conference 继承）
	video_layout_t *vlayout; // 当前生效的布局配置
	video_layout_t *new_vlayout; // 待切换的新布局（过渡用）
	int canvas_id;    // 画布编号（0 到 canvas_count-1）
	struct conference_obj *conference; // 所属会议的反向引用
	switch_thread_t *video_muxing_thread;  // 融合主线程句柄
	int video_timer_reset;    // 标志：是否需要重置帧率定时器
	switch_queue_t *video_queue;  // 视频帧队列（多画布时传递给 super canvas）
	int recording;  // 是否正在录像（写入文件）
	switch_image_t *bgimg;  // 背景图片（如会议背景图/PNG）
	switch_image_t *fgimg;   // 前景图片（如水印、Logo，半透明叠加）
	int playing_video_file; // 是否正在播放视频文件（全屏覆盖）
	int overlay_video_file; // 是否以叠加模式播放视频文件（半透明覆盖）
	codec_set_t *write_codecs[MAX_MUX_CODECS];  // 编码器集合（按 codec 分组）
	int write_codecs_count;  // 已注册的编码器数量
	switch_bool_t disable_auto_clear;  // 禁止自动清除图层（保留上一帧）
} mcu_canvas_t;

/* Record Node */
typedef struct conference_record {
	struct conference_obj *conference;
	char *path;
	switch_memory_pool_t *pool;
	switch_bool_t autorec;
	struct conference_record *next;
	switch_file_handle_t fh;
	int canvas_id;
} conference_record_t;

typedef enum {
	CONF_VIDEO_MODE_PASSTHROUGH,
	CONF_VIDEO_MODE_TRANSCODE,
	CONF_VIDEO_MODE_MUX
} conference_video_mode_t;

/* Conference Object */
typedef struct conference_obj {
	char *name; // 会议名称
	char *la_name; // Live Array 名称，从 name 中去掉 @ 后缀得到，用于创建实时事件数组的频道名
	char *la_event_channel; // 事件通道名，Live Array 状态变更通过此通道推送
	char *chat_event_channel; // 聊天消息事件通道
	char *mod_event_channel; // 主持人操作事件通道
	char *info_event_channel; // 会议信息查询事件通道
	char *desc; // 会议描述文字（可选）
	char *timer_name; // 定时器源名称，如 "soft"，用于音频帧同步
	char *tts_engine; // TTS 引擎名称，如 "flite"、"google"
	char *tts_voice;  //TTS 语音名称
	char *member_enter_sound; // 成员进入时播放给该成员听的音效
	char *enter_sound; // 成员进入时播放给所有人听的音效
	char *exit_sound; // 成员离开时播放给所有人听的音效
	char *alone_sound; // 只有 1 个人在会议时周期播放的音效
	char *perpetual_sound; // 无条件循环播放的背景音，无论有几个人都播
	char *moh_sound; // Music-on-Hold，仅当只有 1 人或等待主持人时播放
	char *tmp_moh_sound; // 临时 MOH 覆盖，优先级高于 moh_sound
	char *muted_sound; // 成员被静音时播放
	char *mute_detect_sound; //检测到成员静音时播放
	char *unmuted_sound; // 成员取消静音时播放
	char *deaf_sound; // 成员被设为"聋"（听不到）时播放
	char *undeaf_sound; // 成员取消"聋"时播放
	char *blind_sound; // 盲转时播放
	char *unblind_sound; //取消盲转时播放
	char *locked_sound; // 会议被锁定时播放
	char *is_locked_sound; // 新成员加入已锁定的会议时播放
	char *is_unlocked_sound; // 会议解锁时播放
	char *kicked_sound; // 成员被踢出时播放
	char *join_only_sound; // 只加入模式提示音，加入会议时播放给该成员听，告知其已进入但无法参与互动
	char *caller_id_name; // 会议对外呼出时的 Caller ID 名称
	char *caller_id_number; // 会议对外呼出时的 Caller ID 号码
	char *sound_prefix; // 音效文件的根路径前缀，如 "/usr/share/freeswitch/sounds/en/us/callie/"
	char *special_announce;
	char *auto_record; // 自动录音模板路径（如 "${record_base_dir}/${conference_name}.wav"）
	int auto_record_canvas; // 自动录音使用的画布 ID（多画布时指定录哪个）
	char *record_filename; // 当前录音文件名
	char *outcall_templ; // 外呼模板
	char *video_layout_conf; // 视频布局配置字符串
	char *video_layout_name; // 当前使用的布局名称
	char *video_layout_group; // 当前布局组名称（组内包含多个布局，按人数自动切换）
	char *video_canvas_bgcolor; // 画布背景色（如 "#000000"）
	char *video_canvas_bgimg; // 	画布背景图片路径
	char *video_border_color; // 视频边框颜色
	char *video_super_canvas_bgcolor; // 超级画布背景色
	char *video_letterbox_bgcolor; // Letterbox 填充色（头像/视频不匹配层大小时的填充色）
	char *video_mute_banner; // 视频静音横幅文字（如 "MUTED"）
	char *no_video_avatar; // 无视频时的默认头像图片路径
	switch_event_t *variables; // 会议级自定义变量字典（可通过 channel variable 读写）
	conference_video_mode_t conference_video_mode; // 视频模式：CONF_VIDEO_MODE_PASSTHROUGH（透传）或 CONF_VIDEO_MODE_MUX（混频）
	int video_quality; // 视频质量参数（影响编码码率）
	int members_with_video; // 有视频的成员数（实时更新）
	int members_seeing_video; // 正在观看视频的成员数
	int members_with_avatar; // 有头像（无视频）的成员数
	uint32_t auto_kps_debounce; // 自动码率降级的防抖时间（ms），默认 5000ms
	switch_codec_settings_t video_codec_settings; // 视频编解码器设置（编码参数）
	uint32_t canvas_width; //画布默认宽度
	uint32_t canvas_height; // 画布默认高度
	uint32_t terminate_on_silence; // 静音多少秒后自动结束会议
	uint32_t max_members; // 最大允许成员数，0=无限制
	uint32_t doc_version; // 文档/状态版本号（用于前端同步）
	uint32_t video_border_size; // 视频边框大小（像素）
	char *maxmember_sound; // 达到最大人数时播放的音效
	uint32_t announce_count; // 达到多少人时播报人数
	char *pin; // 普通成员的 PIN 码
	char *mpin; // 管理员的 PIN 码
	char *pin_sound; // 提示输入 PIN 的音效
	char *bad_pin_sound; // PIN 输入错误的音效
	char *profile_name; // 所使用的会议配置 profile 名称（如 "default"、"wideband"）
	char *domain; // 会议所属的 SIP 域名
	char *chat_id; // 	聊天 ID
	char *caller_controls; // 普通成员的 DTMF 控制映射表名称
	char *moderator_controls;// 主持人的 DTMF 控制映射表名称
	switch_live_array_t *la; // Live Array 对象，实时跟踪成员状态并推送给前端客户端
	conference_flag_t flags[CFLAG_MAX]; // 会议级标志位数组，控制全局行为（如 CFLAG_DESTRUCT、CFLAG_VIDEO_MUXING、CFLAG_MINIMIZE_VIDEO_ENCODING、CFLAG_PERSONAL_CANVAS 等）
	member_flag_t mflags[MFLAG_MAX]; // 默认成员标志位——新成员加入时继承这些标志
	switch_call_cause_t bridge_hangup_cause; // 桥接挂断原因码
	switch_mutex_t *flag_mutex; // 保护标志位操作的互斥锁
	switch_mutex_t *file_mutex; // 保护文件节点的互斥锁
	uint32_t rate; // 音频采样率
	uint32_t interval; //每帧音频的时长（ms），通常 20ms。决定了音频混音循环的节拍
	uint32_t channels; // 声道数，1=单声道，2=立体声
	switch_mutex_t *mutex; // 会议主互斥锁
	conference_member_t *members; // 成员链表头指针，所有成员以单链表串联
	uint32_t floor_holder; // 当前音频发言权持有者的成员 ID
	uint32_t video_floor_holder; // 当前视频发言权持有者的成员 ID（占据主画面位置）
	uint32_t last_video_floor_holder; // 上一个视频发言权持有者的 ID
	switch_mutex_t *member_mutex; // 保护 members 链表的互斥锁
	conference_file_node_t *fnode; //同步文件播放节点（当前正在播放的同步音频/视频文件）
	conference_file_node_t *async_fnode; // 异步文件播放节点（异步播放，不阻塞主循环）
	switch_memory_pool_t *pool; // 会议的内存池（APR pool），所有会议内存从中分配，销毁时统一释放
	switch_thread_rwlock_t *rwlock; // 读写锁，保护会议不被意外销毁
	uint32_t count; // 当前成员总数
	int32_t energy_level; // 	语音检测阈值。成员音频能量超过此值才被认为"在说话"
	int32_t auto_energy_level; // 自动模式下的能量阈值
	int32_t max_energy_level; // 能量上限（防爆音）
	uint32_t agc_level; // AGC 目标音量级别
	uint32_t agc_low_energy_level; // AGC 低能量阈值（低于此值才增益）
	uint32_t agc_margin; // AGC 调整裕度
	uint32_t agc_change_factor; // AGC 每次调整的幅度因子
	uint32_t agc_period_len; // AGC 调整周期的帧数
	int32_t max_energy_hit_trigger; // 能量超过最大值多少次后触发处理
	int32_t auto_energy_sec; // 	自动能量检测的秒数窗口
	uint32_t burst_mute_count; // 突发音量导致的静音计数
	uint8_t min; // 最小编解码器间隔（ms），用于编解码协商
	switch_speech_handle_t lsh; // 内嵌的 speech handle（局部使用）
	switch_speech_handle_t *sh; // 指向当前使用的 speech handle
	switch_byte_t *not_talking_buf; // "静音"帧缓冲区——所有成员都没人说话时输出的音频数据
	uint32_t not_talking_buf_len; // "静音"帧缓冲区长度
	int pin_retries; // PIN 输入最大重试次数
	int broadcast_chat_messages; // 是否广播聊天消息
	int comfort_noise_level; // 舒适噪声级别（静音时填充的低级别噪声，避免完全无声的"死寂"感）
	int auto_recording; // 自动录音是否已启动标志
	char *recording_metadata; // 录音元数据
	int record_count; // 录音计数
	uint32_t min_recording_participants; // 最少录音参与者人数（少于此数不录）
	int ivr_dtmf_timeout; // IVR DTMF 输入超时（ms）
	int ivr_input_timeout; // IVR 总输入超时（ms）
	uint32_t eflags; // 	启用的事件类型位掩码
	uint32_t verbose_events; // 是否发送详细事件
	int end_count; // 具有 endconf 权限的成员计数
	uint32_t count_ghosts; // 	"幽灵"成员数（半连接/残留状态的成员）
	/* allow extra time after 'endconf' member leaves */
	switch_time_t endconference_time; // 	endconf 成员离开后的宽限起始时间
	int endconference_grace_time; // 宽限时间（秒），最后一个 endconf 成员离开后等待多久再结束会议

	uint32_t relationship_total; // 	成员间自定义关系（如A静音B）的总数
	uint32_t score; // 会议级别的原始能量分数（当前未广泛使用）
	int mux_loop_count; // 混音循环计数（当前主要作为诊断/调试用）
	int member_loop_count; // 每个混音周期处理的成员数
	switch_time_t run_time; // 会议运行时长
	char *uuid_str; // 创建该会议的原始呼叫 UUID
	uint32_t originating; // 正在发起的外呼数量
	switch_call_cause_t cancel_cause; // 取消原因码
	conference_cdr_node_t *cdr_nodes; // CDR（呼叫详情记录）节点链表
	conference_cdr_reject_t *cdr_rejected; // 被拒绝的加入记录
	switch_time_t start_time; // 会议创建时间
	switch_time_t end_time; // 会议结束时间
	char *log_dir; // CDR 日志目录
	cdr_event_mode_t cdr_event_mode; // CDR 事件模式 （CDRE_NONE=不生成事件, CDRE_AS_CONTENT=事件内容包含 CDR 数据, CDRE_AS_FILE=事件包含 CDR 文件路径）
	struct vid_helper vh[2]; // 视频线程辅助数组（2个），用于跟踪视频线程状态并实现优雅关闭
	struct vid_helper mh; // 成员级视频辅助（当前未使用，为预留字段）
	conference_record_t *rec_node_head; // 录音节点链表头（支持多个并发录音）
	int last_speech_channels; // 上一次 TTS 输出的声道数。声道配置变化时需要重新打开 speech handle
	mcu_canvas_t *canvases[MAX_CANVASES+1]; // 画布数组，每个画布是一个独立的视频混频单元
	uint32_t canvas_count; // 当前画布数量
	int super_canvas_label_layers; // 是否在超级画布的各层上显示"Canvas N"标签
	int super_canvas_show_all_layers; // 是否在超级画布上显示所有子画布（包括空闲的）
	int canvas_running_count; // 正在运行的画布线程数
	switch_mutex_t *canvas_mutex; // 保护画布操作的互斥锁
	switch_hash_t *layout_hash; // 布局名称→布局定义的哈希表
	switch_hash_t *layout_group_hash;// 布局组名称→布局组的哈希表
	switch_fps_t video_fps; // 视频帧率配置（包含 fps、ms、samples）
	int recording_members; // 正在录制视频的成员数
	uint32_t video_floor_packets; // 视频发言权转移所需的最小连续数据包数（防闪切）
	video_layout_t *new_personal_vlayout; // 待应用的新个人画布布局（个人画布模式下使用）
	int max_bw_in; // 最大入站视频带宽（kbps）
	int force_bw_in; // 强制入站视频带宽（0=不强制）

	/* special use case, scalling shared h264 canvas*/
	int scale_h264_canvas_width; // H264 专用缩放目标宽度（降低编码分辨率以节省带宽）
	int scale_h264_canvas_height; // 	H264 专用缩放目标高度
	int scale_h264_canvas_fps_divisor; // H264 帧率除数（如 2=降为原来一半帧率）
	char *scale_h264_canvas_bandwidth; // H264 缩放后的目标带宽（字符串，如 "auto" 或 "512k"）
	uint32_t moh_wait; // MOH 重试冷却计数器（以音频帧数为单位）。MOH 播放失败时设为 2000/interval（约2秒），倒计到 0 才允许再次尝试
	uint32_t floor_holder_score_iir; // 当前发言权持有者的 IIR 平滑能量值
	char *default_layout_name; // 默认布局名称（重置时恢复用）
	int mux_paused; // 混频暂停标志（如两人桥接模式时不做混频）
	char *video_codec_config_profile_name; // 视频编码配置 profile 名称
	int heartbeat_period_sec; // 心跳周期（秒），定期发送会议状态事件
} conference_obj_t;

/* Relationship with another member */
typedef struct conference_relationship {
	uint32_t id;
	uint32_t flags;
	struct conference_relationship *next;
} conference_relationship_t;

/***
 *conference_member_t (一个参会者)
    │
    ├── session → switch_core_session (SIP 呼叫会话)
    │     └── channel → switch_channel (通道，读写变量/标志)
    │
    ├── 音频路径 ──────────────────────────────────────────┐
    │   RTP → audio_buffer → [read_resampler] → frame     │
    │         (audio_in_mutex 保护)                         │
    │                                                       │
    │   mux_buffer → 编码 → RTP 发送                        │
    │   (audio_out_mutex 保护)                              │
    │                                                       │
    │   能量: frame → score → score_iir → gate_open        │
    │         码率管理: managed_kps ──请求──→ 对端           │
    └──────────────────────────────────────────────────────┘
    │
    ├── 视频路径 ──────────────────────────────────────────┐
    │   RTP → video_queue → pop_next_image() → img         │
    │                                                       │
    │   video_layer_id → canvas->layers[id] (层位置)       │
    │     └── img → scale_and_patch → canvas->img (合成)    │
    │                                                       │
    │   帧质量: good_img / blanks / blackouts               │
    │   视频: flip (旋转), video_filters (滤镜)             │
    │   摄像头: cam_opts (自动变焦/平移)                    │
    │                                                       │
    │   fb (帧缓冲区) → video_muxing_write_thread → RTP    │
    └──────────────────────────────────────────────────────┘
    │
    ├── relationships → [rel1(id=3,flags=静音)] → [rel2] → NULL
    │
    ├── fnode → 成员级音频文件播放
    │
    └── al → 空间音频处理

 *
 *
 */

/* Conference Member Object */
struct conference_member {
	uint32_t id; //成员唯一 ID（在会议内递增分配），是所有成员操作（踢人、静音等）的索引
	switch_core_session_t *session; // FreeSWITCH 会话对象，代表该成员的 SIP 呼叫。为 NULL 时表示"幽灵"成员（录音节点等）
	switch_channel_t *channel; // 会话的通道对象，用于读写通道变量和状态标志
	conference_obj_t *conference; // 	反向指向所属会议对象
	switch_memory_pool_t *pool; // 成员专属内存池，成员退出时统一释放
	switch_buffer_t *audio_buffer; // 输入音频缓冲区。从 RTP 读取的音频帧暂存在此，等待混音线程消费
	switch_buffer_t *mux_buffer;  // 输出混音缓冲区。混音完成后，该成员应该听到的混合音频写入此缓冲区
	switch_buffer_t *resample_buffer; // 重采样中间缓冲区。当成员采样率与会议不同时，重采样过程中的中间数据
	member_flag_t flags[MFLAG_MAX]; // 成员标志位数组。关键标志包括：MFLAG_CAN_SPEAK（能说话）、MFLAG_CAN_BE_SEEN（视频可见）、MFLAG_CAN_HEAR（能听到）、MFLAG_HOLD（保持）、MFLAG_RUNNING（线程运行中）等
	int32_t score; // 当前帧的音频能量值（所有采样绝对值之和 / 采样数）。每帧实时计算
	int32_t last_score; // 上一帧的能量值，用于计算差值
	uint32_t score_iir; // IIR 平滑后的能量值。公式：(1-DECAY)*score + DECAY*score_iir，防止瞬时波动
	switch_mutex_t *flag_mutex; // 保护标志位的互斥锁
	switch_mutex_t *write_mutex; // 保护写操作
	switch_mutex_t *audio_in_mutex; // 保护 audio_buffer（输入端写入，混音线程读取）
	switch_mutex_t *audio_out_mutex; // 保护 mux_buffer（混音线程写入，输出端读取）
	switch_mutex_t *read_mutex; // 保护读操作
	switch_mutex_t *fnode_mutex; // 保护成员级文件播放节点
	switch_thread_rwlock_t *rwlock; // 读写锁，其他线程引用该成员时加读锁，防止成员被销毁
	switch_codec_implementation_t read_impl; // 当前读编解码器的实现参数（采样率、打包间隔等）
	switch_codec_implementation_t orig_read_impl; // 原始读编解码器参数（保存初始值，用于恢复）
	switch_codec_t read_codec; // 读编解码器实例
	switch_codec_t write_codec; // 	写编解码器实例
	char *rec_path; // 录像文件路径
	switch_time_t rec_time; // 录像开始时间
	conference_record_t *rec; // 录像节点对象
	/**add new member record */
	char * member_record_path; //成员单独录像路径
	switch_bool_t member_record; // 成员单独录像开关
	/**-------- */
	uint8_t *frame; // 原始音频帧缓冲区（从 audio_buffer 读取的一帧数据），frame_size 字节
	uint8_t *last_frame; //上一帧音频（用于计算能量差值 score_delta_accum，当前代码中未广泛使用）
	uint32_t frame_size; // frame 缓冲区的大小（字节）
	uint8_t *mux_frame; // 	混合输出帧缓冲区（备用/遗留字段）
	uint32_t read; // 当前读取的字节数
	uint32_t vol_period; // 音量调整后的冷却帧数。调整音量后跳过若干帧再恢复正常检测
	int32_t energy_level; // 语音检测阈值。score > energy_level 时认为在说话
	int32_t auto_energy_level; // 自动调整模式下的能量阈值
	int32_t max_energy_level; // 能量上限（防爆音）
	int32_t agc_level; // AGC 目标音量级别
	uint32_t agc_low_energy_level; // 	AGC 低能量阈值
	uint32_t agc_margin; // AGC 调整裕度
	uint32_t agc_change_factor; // 每次增益调整的幅度因子
	uint32_t agc_period_len; // 	AGC 调整周期（帧数）
	switch_agc_t *agc; // FreeSWITCH 内置 AGC 算法状态
	uint32_t mute_counter; // 连续低能量帧计数
	uint32_t burst_mute_count; // 突发音量触发静音的计数
	uint32_t score_avg; // 本次说话期间的平均能量（score_accum / score_count）
	uint32_t max_energy_hits; // 连续超过最大能量的帧数
	uint32_t max_energy_hit_trigger; // 超过最大能量多少帧后触发处理
	int32_t volume_in_level; // 输入音量增益（从成员收取的音频放大/缩小）
	int32_t volume_out_level; // 输出音量增益（发给成员的音频放大/缩小）
	switch_time_t join_time; // 加入会议的时间戳
	time_t last_talking; // 上一次停止说话的时间
	switch_time_t first_talk_detect; // 第一次检测到说话的时间戳
	uint32_t talk_detects; // 检测到的说话次数（每次从静音→说话算一次）
	uint32_t auto_energy_track; // 非说话期间的帧计数。超过 auto_energy_sec 秒后自动降低 energy_level（自适应阈值）
	uint32_t talk_track; // 帧计数器，累积到约 10 秒时触发一次 talk-data 事件，报告说话统计
	uint32_t score_count; // 说话期间的帧计数
	uint32_t score_accum; // 说话期间的能量累计总和
	uint32_t score_delta_accum; // 说话期间的能量差值累计（abs(score - last_score)），衡量声音稳定性
	uint32_t native_rate; // 成员的原生音频采样率
	uint32_t gate_open; // 当前帧噪声门状态：1=能量超过阈值（在说话），0=低于阈值（静音）
	uint32_t gate_count; // 本次说话期间通过噪声门的帧数（能量超标的帧）
	uint32_t nogate_count; // 本次说话期间未通过噪声门的帧数（能量不足但仍处于说话状态，因为有余晖机制）
	uint32_t talking_count; // 说话次数累计
	switch_audio_resampler_t *read_resampler; // 音频重采样器。将成员音频从其原生采样率转换到会议统一采样率
	int16_t *resample_out; // 重采样输出缓冲区
	uint32_t resample_out_len; // 重采样输出缓冲区长度
	conference_file_node_t *fnode; // 成员级文件播放节点（仅对该成员播放的音频/视频文件）
	conference_relationship_t *relationships; // 与其他成员的关系链表。如"A 静音 B"、"A 听不到 C"
	switch_speech_handle_t lsh; // 内嵌 TTS 句柄
	switch_speech_handle_t *sh; // 指向当前 TTS 句柄
	uint32_t verbose_events; // 是否为该成员发送详细事件
	struct conference_member *next; // 链表后向指针，串联到下一个成员
	switch_ivr_dmachine_t *dmachine; // DTMF 状态机（检测拨号方案匹配）
	conference_cdr_node_t *cdr_node; // 该成员的 CDR（呼叫详情记录）节点
	char *kicked_sound; // 该成员被踢出时播放的音效
	switch_queue_t *dtmf_queue; // DTMF 事件队列
	switch_queue_t *video_queue; //视频帧输入队列（从 RTP 收到的视频帧暂存）
	switch_thread_t *video_muxing_write_thread; // 视频写入线程（将编码后的帧写入 RTP）
	switch_thread_t *video_layer_thread; // 层处理线程（多核时并行做缩放贴图）
	int layer_thread_running; // 	层处理线程是否运行中
	switch_thread_t *input_thread; // 视频输入线程
	switch_thread_cond_t *layer_cond; // 层线程的条件变量（唤醒层线程处理新帧）
	switch_mutex_t *layer_cond_mutex; // 保护条件变量的互斥锁
	cJSON *json; //成员状态的 JSON 表示（用于 Live Array 推送）
	cJSON *status_field; // 状态字段的 JSON 对象
	uint8_t loop_loop; // 成员音频循环的退出标志。设为 1 时循环线程退出
	al_handle_t *al; // 空间音频处理句柄。启用时，成员的音频会根据其"位置"进行空间化处理（模拟 3D 声场）
	int last_speech_channels; // 上次 TTS 输出的声道数。声道数变化时需重新打开 speech handle
	int video_layer_id; // 当前分配的视频层 ID（-1=未分配）。层是画布上的一个视频窗口位置
	int canvas_id; // 当前分配到的画布 ID（-1=未分配）
	int watching_canvas_id; // 	正在观看的画布 ID。决定成员看到哪个画布的合成画面
	int layer_timeout; // 层分配超时计数器。连续多帧无法分配到层时尝试切换画布
	int video_codec_index; // 	在画布 write_codecs[] 数组中的索引（minimize encoding 分组）
	int video_codec_id; // 成员使用的编解码器 ID
	char *video_banner_text; // 视频横幅文字（如成员名称，显示在视频层上）
	switch_image_t *video_logo; // 成员的视频 Logo 叠加图
	switch_img_position_t logo_pos;  // Logo 位置（左上/右下等）
	switch_img_fit_t logo_fit; //	Logo 缩放模式
	char *video_mute_png; // 视频静音图片文件路径
	char *video_reservation_id; // 视频层预留 ID（通过 vid-reservation-id 保留特定层位置）
	char *video_role_id; // 视频角色 ID（如 "presenter"、"audience"，绑定到有对应 role_id 的层）
	char *video_codec_group; // 编解码器分组名称。同组内的成员共享编码输出
	switch_vid_params_t vid_params; // 视频参数（分辨率、帧率等）
	uint32_t auto_kps_debounce_ticks; // 码率降级的防抖倒计时（帧数）。倒计到 0 才真正降低码率
	uint32_t layer_loops; // 层循环计数，用于自动码率检测
	switch_frame_buffer_t *fb; // 帧缓冲区，视频写入线程从中取帧编码发送
	switch_image_t *avatar_png_img; // 成员的头像图片（无视频时显示）
	switch_image_t *video_mute_img; // 视频静音时的冻结帧截图
	uint32_t floor_packets;
	int blanks; // 连续空帧计数。达到 fps 时请求视频刷新（I 帧请求）
	int managed_kps; // 当前请求的入站视频码率（kbps）
	int managed_kps_set; // 已确认发送给对端的码率。避免重复发送相同码率请求
	int blackouts; // 严重黑屏事件计数。达到 fps*5 时显示头像并清除码率管理
	int good_img; // 	连续有效（非空）视频帧计数。每 fps*10 帧重置比特率计数器
	int auto_avatar; // 自动头像检测标志
	int avatar_patched; // 头像已贴到层的标志（避免重复操作）
	switch_media_flow_t video_media_flow; // 视频媒体流方向（SENDRECV、SENDONLY、RECVONLY、INACTIVE）
	mcu_canvas_t *canvas; // 个人画布指针（Personal Canvas 模式下使用）
	switch_image_t *pcanvas_img; // 个人画布模式下，从该成员获取的视频帧
	int max_bw_in; // 该成员的最大入站带宽限制
	int force_bw_in; // 强制入站带宽
	int max_bw_out; // 	该成员的最大出站带宽限制
	int reset_media; //媒体重置倒计时（帧数）。检测到 CF_CONFERENCE_RESET_MEDIA 时设为 10，倒计到 0 时调用 conference_member_setup_media() 重新初始化媒体
	int flip; // 视频旋转角度（0/90/180/270 度）
	int flip_count; // 自动旋转的帧计数器。达到 fps/2 时旋转 90 度

	switch_mutex_t *text_mutex; // 保护文本缓冲区
	switch_buffer_t *text_buffer; // 文本数据缓冲区（累积 T.140 实时文本）
	char *text_framedata; // 组装后的完整文本帧数据（初始 1024 字节，按需扩容)
	uint32_t text_framesize; // text_framedata 的当前分配大小

	mcu_layer_cam_opts_t cam_opts; // 摄像头控制选项：自动变焦（autozoom）、自动平移（autopan）、变焦因子（zoom_factor）、平移速度等
	switch_core_video_filter_t video_filters; // 视频滤镜位掩码（灰度 SCV_FILTER_GRAY_FG、复古 SCV_FILTER_SEPIA_FG、8位 SCV_FILTER_8BIT_FG 等）
	int video_manual_border; // 手动视频边框大小

};

typedef enum {
	CONF_API_SUB_ARGS_SPLIT, //把命令参数按空格拆成 argc / argv，再交给处理函数 参数是多个独立字段的命令，比如 list、play、record、setvar
	CONF_API_SUB_MEMBER_TARGET, //第一个参数先被当成“成员选择器”，再把命令作用到一个或多个成员上   先选成员，再执行命令
	CONF_API_SUB_ARGS_AS_ONE  //  后面的整段文本保持原样
} conference_fntype_t;

typedef void (*void_fn_t) (void);

/* API command parser */
typedef struct api_command {
	char *pname;
	void_fn_t pfnapicmd;
	conference_fntype_t fntype;
	char *pcommand;
	char *psyntax;
} api_command_t;

typedef void (*conference_key_callback_t) (conference_member_t *, struct caller_control_actions *);

typedef struct {
	conference_member_t *member;
	caller_control_action_t action;
	conference_key_callback_t handler;
} key_binding_t;

struct _mapping {
	const char *name;
	conference_key_callback_t handler;
};

typedef enum {
	CONF_API_COMMAND_LIST = 0,
	CONF_API_COMMAND_ENERGY,
	CONF_API_COMMAND_VOLUME_IN,
	CONF_API_COMMAND_VOLUME_OUT,
	CONF_API_COMMAND_PLAY,
	CONF_API_COMMAND_SAY,
	CONF_API_COMMAND_SAYMEMBER,
	CONF_API_COMMAND_STOP,
	CONF_API_COMMAND_DTMF,
	CONF_API_COMMAND_KICK,
	CONF_API_COMMAND_MUTE,
	CONF_API_COMMAND_UNMUTE,
	CONF_API_COMMAND_DEAF,
	CONF_API_COMMAND_UNDEAF,
	CONF_API_COMMAND_RELATE,
	CONF_API_COMMAND_LOCK,
	CONF_API_COMMAND_UNLOCK,
	CONF_API_COMMAND_DIAL,
	CONF_API_COMMAND_BGDIAL,
	CONF_API_COMMAND_TRANSFER,
	CONF_API_COMMAND_RECORD,
	CONF_API_COMMAND_NORECORD,
	CONF_API_COMMAND_EXIT_SOUND,
	CONF_API_COMMAND_ENTER_SOUND,
	CONF_API_COMMAND_PIN,
	CONF_API_COMMAND_NOPIN,
	CONF_API_COMMAND_GET,
	CONF_API_COMMAND_SET,
} api_command_type_t;

struct bg_call {
	conference_obj_t *conference;
	switch_core_session_t *session;
	char *bridgeto;
	uint32_t timeout;
	char *flags;
	char *cid_name;
	char *cid_num;
	char *conference_name;
	char *uuid;
	char *profile;
	switch_call_cause_t *cancel_cause;
	switch_event_t *var_event;
	switch_memory_pool_t *pool;
};

/* FUNCTION DEFINITIONS */


switch_bool_t conference_utils_test_flag(conference_obj_t *conference, conference_flag_t flag);
conference_relationship_t *conference_member_get_relationship(conference_member_t *member, conference_member_t *other_member);

uint32_t next_member_id(void);
void conference_utils_set_cflags(const char *flags, conference_flag_t *f);
void conference_utils_set_mflags(const char *flags, member_flag_t *f);
void conference_utils_merge_mflags(member_flag_t *a, member_flag_t *b);
void conference_utils_clear_eflags(char *events, uint32_t *f);
void conference_event_pres_handler(switch_event_t *event);
void conference_data_event_handler(switch_event_t *event);
void conference_event_call_setup_handler(switch_event_t *event);
void conference_member_add_file_data(conference_member_t *member, int16_t *data, switch_size_t file_data_len);
void conference_send_notify(conference_obj_t *conference, const char *status, const char *call_id, switch_bool_t final);
switch_status_t conference_file_close(conference_obj_t *conference, conference_file_node_t *node);
void *SWITCH_THREAD_FUNC conference_record_thread_run(switch_thread_t *thread, void *obj);
switch_status_t conference_close_open_files(conference_obj_t *conference);
void conference_al_gen_arc(conference_obj_t *conference, switch_stream_handle_t *stream);
void conference_al_process(al_handle_t *al, void *data, switch_size_t datalen, int rate);

void conference_utils_member_set_flag_locked(conference_member_t *member, member_flag_t flag);
void conference_utils_member_set_flag(conference_member_t *member, member_flag_t flag);

void conference_member_update_status_field(conference_member_t *member);
void conference_video_vmute_snap(conference_member_t *member, switch_bool_t clear);
void conference_video_reset_video_bitrate_counters(conference_member_t *member);
void conference_video_clear_layer(mcu_layer_t *layer);
int conference_member_get_canvas_id(conference_member_t *member, const char *val, switch_bool_t watching);
void conference_video_reset_member_codec_index(conference_member_t *member);
void conference_video_detach_video_layer(conference_member_t *member);
void conference_utils_set_flag(conference_obj_t *conference, conference_flag_t flag);
void conference_utils_set_flag_locked(conference_obj_t *conference, conference_flag_t flag);
void conference_utils_clear_flag(conference_obj_t *conference, conference_flag_t flag);
void conference_utils_clear_flag_locked(conference_obj_t *conference, conference_flag_t flag);
switch_status_t conference_loop_dmachine_dispatcher(switch_ivr_dmachine_match_t *match);

mcu_layer_t *conference_video_get_layer_locked(conference_member_t *member);
void conference_video_release_layer(mcu_layer_t **layer);
mcu_canvas_t *conference_video_get_canvas_locked(conference_member_t *member);
void conference_video_release_canvas(mcu_canvas_t **canvasP);
switch_status_t conference_video_change_res(conference_obj_t *conference, int w, int h, int id);
int conference_member_setup_media(conference_member_t *member, conference_obj_t *conference);

al_handle_t *conference_al_create(switch_memory_pool_t *pool);
switch_status_t conference_member_parse_position(conference_member_t *member, const char *data);
video_layout_t *conference_video_find_best_layout(conference_obj_t *conference, layout_group_t *lg, uint32_t count, uint32_t file_count);
void conference_list_count_only(conference_obj_t *conference, switch_stream_handle_t *stream);
void conference_member_set_floor_holder(conference_obj_t *conference, conference_member_t *member, uint32_t id);
void conference_utils_member_clear_flag(conference_member_t *member, member_flag_t flag);
void conference_utils_member_clear_flag_locked(conference_member_t *member, member_flag_t flag);
switch_status_t conference_video_attach_video_layer(conference_member_t *member, mcu_canvas_t *canvas, int idx);
int conference_video_set_fps(conference_obj_t *conference, float fps);
void conference_member_set_logo(conference_member_t *member, const char *path);
void conference_video_layer_set_logo(conference_member_t *member, mcu_layer_t *layer);
void conference_video_layer_set_banner(conference_member_t *member, mcu_layer_t *layer, const char *text);
void conference_fnode_seek(conference_file_node_t *fnode, switch_stream_handle_t *stream, char *arg);
uint32_t conference_member_stop_file(conference_member_t *member, file_stop_t stop);
switch_bool_t conference_utils_member_test_flag(conference_member_t *member, member_flag_t flag);
void conference_list_pretty(conference_obj_t *conference, switch_stream_handle_t *stream);
switch_status_t conference_record_stop(conference_obj_t *conference, switch_stream_handle_t *stream, char *path);
switch_status_t conference_record_action(conference_obj_t *conference, char *path, recording_action_type_t action);
void conference_xlist(conference_obj_t *conference, switch_xml_t x_conference, int off);
void conference_jlist(conference_obj_t *conference, cJSON *json_conferences);
void conference_event_send_json(conference_obj_t *conference);
void conference_event_send_rfc(conference_obj_t *conference);
void conference_member_update_status_field(conference_member_t *member);
void conference_event_la_command_handler(switch_live_array_t *la, const char *cmd, const char *sessid, cJSON *jla, void *user_data);
void conference_event_adv_la(conference_obj_t *conference, conference_member_t *member, switch_bool_t join);
void conference_event_adv_layout(conference_obj_t *conference, mcu_canvas_t *canvas, video_layout_t *vlayout);
switch_status_t conference_video_init_canvas(conference_obj_t *conference, video_layout_t *vlayout, mcu_canvas_t **canvasP);
switch_status_t conference_video_attach_canvas(conference_obj_t *conference, mcu_canvas_t *canvas, int super);
void conference_video_init_canvas_layers(conference_obj_t *conference, mcu_canvas_t *canvas, video_layout_t *vlayout, switch_bool_t force);
switch_status_t conference_video_attach_video_layer(conference_member_t *member, mcu_canvas_t *canvas, int idx);
void conference_video_reset_video_bitrate_counters(conference_member_t *member);
void conference_video_layer_set_banner(conference_member_t *member, mcu_layer_t *layer, const char *text);
void conference_video_detach_video_layer(conference_member_t *member);
void conference_video_check_used_layers(mcu_canvas_t *canvas);
void conference_video_check_flush(conference_member_t *member, switch_bool_t force);
void conference_video_set_canvas_letterbox_bgcolor(mcu_canvas_t *canvas, char *color);
void conference_video_set_canvas_bgcolor(mcu_canvas_t *canvas, char *color);
void conference_video_scale_and_patch(mcu_layer_t *layer, switch_image_t *ximg, switch_bool_t freeze);
void conference_video_reset_layer(mcu_layer_t *layer);
void conference_video_reset_layer_cam(mcu_layer_t *layer);
void conference_video_clear_layer(mcu_layer_t *layer);
void conference_video_reset_image(switch_image_t *img, switch_rgb_color_t *color);
void conference_video_parse_layouts(conference_obj_t *conference, int WIDTH, int HEIGHT);
int conference_video_set_fps(conference_obj_t *conference, float fps);
video_layout_t *conference_video_get_layout(conference_obj_t *conference, const char *video_layout_name, const char *video_layout_group);
void conference_video_check_avatar(conference_member_t *member, switch_bool_t force);
void conference_video_find_floor(conference_member_t *member, switch_bool_t entering);
void conference_video_destroy_canvas(mcu_canvas_t **canvasP);
void conference_video_fnode_check(conference_file_node_t *fnode, int canvas_id);
switch_status_t conference_video_set_canvas_bgimg(mcu_canvas_t *canvas, const char *img_path);
switch_status_t conference_video_set_canvas_fgimg(mcu_canvas_t *canvas, const char *img_path);
switch_status_t conference_al_parse_position(al_handle_t *al, const char *data);
switch_status_t conference_video_thread_callback(switch_core_session_t *session, switch_frame_t *frame, void *user_data);
switch_status_t conference_text_thread_callback(switch_core_session_t *session, switch_frame_t *frame, void *user_data);
void *SWITCH_THREAD_FUNC conference_video_muxing_write_thread_run(switch_thread_t *thread, void *obj);
void conference_video_launch_layer_thread(conference_member_t *member);
void conference_video_wake_layer_thread(conference_member_t *member);

/* FFmpeg filter functions for user ID/name overlay */
#ifdef HAVE_LIBAVFILTER
switch_status_t conference_video_init_layer_filter(mcu_layer_t *layer, conference_member_t *member, int width, int height);
void conference_video_destroy_layer_filter(mcu_layer_t *layer);
switch_status_t conference_video_apply_layer_filter(mcu_layer_t *layer, switch_image_t **img);
#endif

int conference_member_noise_gate_check(conference_member_t *member);
void conference_member_check_channels(switch_frame_t *frame, conference_member_t *member, switch_bool_t in);

void conference_fnode_toggle_pause(conference_file_node_t *fnode, switch_stream_handle_t *stream);
void conference_fnode_check_status(conference_file_node_t *fnode, switch_stream_handle_t *stream);
void conference_member_set_score_iir(conference_member_t *member, uint32_t score);
// static conference_relationship_t *conference_member_get_relationship(conference_member_t *member, conference_member_t *other_member);
// static void conference_list(conference_obj_t *conference, switch_stream_handle_t *stream, char *delim);

conference_relationship_t *conference_member_add_relationship(conference_member_t *member, uint32_t id);
conference_member_t *conference_member_get(conference_obj_t *conference, uint32_t id);
conference_member_t *conference_member_get_by_str(conference_obj_t *conference, const char *id_str);
conference_member_t *conference_member_get_by_var(conference_obj_t *conference, const char *var, const char *val);
conference_member_t *conference_member_get_by_role(conference_obj_t *conference, const char *role_id);
switch_status_t conference_member_del_relationship(conference_member_t *member, uint32_t id);
switch_status_t conference_member_add(conference_obj_t *conference, conference_member_t *member);
switch_status_t conference_member_del(conference_obj_t *conference, conference_member_t *member);
void *SWITCH_THREAD_FUNC conference_thread_run(switch_thread_t *thread, void *obj);
void *SWITCH_THREAD_FUNC conference_video_muxing_thread_run(switch_thread_t *thread, void *obj);
void *SWITCH_THREAD_FUNC conference_video_super_muxing_thread_run(switch_thread_t *thread, void *obj);
void conference_loop_output(conference_member_t *member);
void conference_loop_launch_input(conference_member_t *member, switch_memory_pool_t *pool);
uint32_t conference_file_stop(conference_obj_t *conference, file_stop_t stop);
switch_status_t conference_file_play(conference_obj_t *conference, char *file, uint32_t leadin, switch_channel_t *channel, uint8_t async);
void conference_member_send_all_dtmf(conference_member_t *member, conference_obj_t *conference, const char *dtmf);
switch_status_t conference_say(conference_obj_t *conference, const char *text, uint32_t leadin);
conference_obj_t *conference_find(char *name, char *domain);
void conference_member_bind_controls(conference_member_t *member, const char *controls);
void conference_send_presence(conference_obj_t *conference);
void conference_video_set_floor_holder(conference_obj_t *conference, conference_member_t *member, switch_bool_t force);
void conference_video_canvas_del_fnode_layer(conference_obj_t *conference, conference_file_node_t *fnode);
void conference_video_canvas_set_fnode_layer(mcu_canvas_t *canvas, conference_file_node_t *fnode, int idx);
void conference_list(conference_obj_t *conference, switch_stream_handle_t *stream, char *delim);
const char *conference_utils_combine_flag_var(switch_core_session_t *session, const char *var_name);
int conference_loop_mapping_len(void);
void conference_api_set_agc(conference_member_t *member, const char *data);

switch_status_t conference_outcall(conference_obj_t *conference,
								   char *conference_name,
								   switch_core_session_t *session,
								   char *bridgeto, uint32_t timeout,
								   char *flags,
								   char *cid_name,
								   char *cid_num,
								   char *profile,
								   switch_call_cause_t *cause,
								   switch_call_cause_t *cancel_cause,
								   switch_event_t *var_event,
								   char** peer_uuid);
switch_status_t conference_outcall_bg(conference_obj_t *conference,
									  char *conference_name,
									  switch_core_session_t *session, char *bridgeto, uint32_t timeout, const char *flags, const char *cid_name,
									  const char *cid_num, const char *call_uuid, const char *profile, switch_call_cause_t *cancel_cause,
									  switch_event_t **var_event);

void conference_video_launch_muxing_thread(conference_obj_t *conference, mcu_canvas_t *canvas, int super);
void conference_launch_thread(conference_obj_t *conference);
void conference_video_launch_muxing_write_thread(conference_member_t *member);
void *SWITCH_THREAD_FUNC conference_loop_input(switch_thread_t *thread, void *obj);
switch_status_t conference_file_local_play(conference_obj_t *conference, switch_core_session_t *session, char *path, uint32_t leadin, void *buf,
										   uint32_t buflen);
switch_status_t conference_member_play_file(conference_member_t *member, char *file, uint32_t leadin, switch_bool_t mux);
switch_status_t conference_member_say(conference_member_t *member, char *text, uint32_t leadin);
uint32_t conference_member_stop_file(conference_member_t *member, file_stop_t stop);
conference_obj_t *conference_new(char *name, conference_xml_cfg_t cfg, switch_core_session_t *session, switch_memory_pool_t *pool);
switch_status_t chat_send(switch_event_t *message_event);


void conference_record_launch_thread(conference_obj_t *conference, char *path, int canvas_id, switch_bool_t autorec);

typedef switch_status_t (*conference_api_args_cmd_t) (conference_obj_t *, switch_stream_handle_t *, int, char **);
typedef switch_status_t (*conference_api_member_cmd_t) (conference_member_t *, switch_stream_handle_t *, void *);
typedef switch_status_t (*conference_api_text_cmd_t) (conference_obj_t *, switch_stream_handle_t *, const char *);

switch_status_t conference_event_add_data(conference_obj_t *conference, switch_event_t *event);
switch_status_t conference_member_add_event_data(conference_member_t *member, switch_event_t *event);

cJSON *conference_cdr_json_render(conference_obj_t *conference, cJSON *req);
char *conference_cdr_rfc4579_render(conference_obj_t *conference, switch_event_t *event, switch_event_t *revent);
void conference_cdr_del(conference_member_t *member);
void conference_cdr_add(conference_member_t *member);
void conference_cdr_rejected(conference_obj_t *conference, switch_channel_t *channel, cdr_reject_reason_t reason);
void conference_cdr_render(conference_obj_t *conference);
void conference_event_channel_handler(const char *event_channel, cJSON *json, const char *key, switch_event_channel_id_t id, void *user_data);
void conference_event_la_channel_handler(const char *event_channel, cJSON *json, const char *key, switch_event_channel_id_t id, void *user_data);
void conference_event_mod_channel_handler(const char *event_channel, cJSON *json, const char *key, switch_event_channel_id_t id, void *user_data);
void conference_event_chat_channel_handler(const char *event_channel, cJSON *json, const char *key, switch_event_channel_id_t id, void *user_data);



void conference_member_itterator(conference_obj_t *conference, switch_stream_handle_t *stream, uint8_t non_mod, conference_api_member_cmd_t pfncallback, void *data);
int conference_video_flush_queue(switch_queue_t *q, int min);

switch_status_t conference_api_sub_canvas_auto_clear(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_mute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_tmute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_unmute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vmute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_tvmute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_unvmute(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vblind(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_tvblind(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_unvblind(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_deaf(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_undeaf(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_video_filter(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_floor(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vid_floor(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_clear_vid_floor(conference_obj_t *conference, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_position(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_conference_video_vmute_snap(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_dtmf(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_pause_play(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_play_status(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_play(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_moh(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_say(conference_obj_t *conference, switch_stream_handle_t *stream, const char *text);
switch_status_t conference_api_sub_dial(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_bgdial(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_auto_position(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_saymember(conference_obj_t *conference, switch_stream_handle_t *stream, const char *text);
switch_status_t conference_api_sub_check_record(conference_obj_t *conference, switch_stream_handle_t *stream, int arc, char **argv);
switch_status_t conference_api_sub_check_record(conference_obj_t *conference, switch_stream_handle_t *stream, int arc, char **argv);
switch_status_t conference_api_sub_volume_in(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_file_seek(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_cam(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_stop(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_hup(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_hold(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_unhold(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_pauserec(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_volume_out(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_getvar(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_setvar(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_lock(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_unlock(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_relate(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_pin(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_exit_sound(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_banner(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_enter_sound(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_set(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_res_id(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_res_id_member(conference_member_t *member, switch_stream_handle_t *stream, char *res_id, int clear, int force);
switch_status_t conference_api_sub_vid_role_id(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_get_uuid(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_get(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_mute_img(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vid_codec_group(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vid_logo_img(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vid_fps(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_res(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_canvas_fgimg(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_canvas_bgimg(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_write_png(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_file_vol(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_recording(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_layout(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_count(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_list(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_xml_list(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_json_list(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_energy(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_auto_energy(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_agc(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_max_energy(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_watching_canvas(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_canvas(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_layer(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_kick(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vid_flip(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_vid_border(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_transfer(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_record(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_norecord(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_bandwidth(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_sub_vid_personal(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv);
switch_status_t conference_api_dispatch(conference_obj_t *conference, switch_stream_handle_t *stream, int argc, char **argv, const char *cmdline, int argn);
switch_status_t conference_api_sub_syntax(char **syntax);
switch_status_t conference_api_main_real(const char *cmd, switch_core_session_t *session, switch_stream_handle_t *stream);
switch_status_t conference_api_set_moh(conference_obj_t *conference, const char *what);

/***add conference member record ***/
switch_status_t conference_api_sub_record_member(conference_member_t *member, switch_stream_handle_t *stream, void *data);
switch_status_t conference_api_sub_stop_record_member(conference_member_t *member, switch_stream_handle_t *stream, void *data);


void conference_loop_mute_on(conference_member_t *member, caller_control_action_t *action);
void conference_loop_mute_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_energy_dn(conference_member_t *member, caller_control_action_t *action);
void conference_loop_energy_equ_conf(conference_member_t *member, caller_control_action_t *action);
void conference_loop_volume_talk_zero(conference_member_t *member, caller_control_action_t *action);
void conference_loop_volume_talk_up(conference_member_t *member, caller_control_action_t *action);
void conference_loop_volume_listen_dn(conference_member_t *member, caller_control_action_t *action);
void conference_loop_lock_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_volume_listen_up(conference_member_t *member, caller_control_action_t *action);
void conference_loop_volume_listen_zero(conference_member_t *member, caller_control_action_t *action);
void conference_loop_volume_talk_dn(conference_member_t *member, caller_control_action_t *action);
void conference_loop_energy_up(conference_member_t *member, caller_control_action_t *action);
void conference_loop_floor_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_vid_floor_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_energy_up(conference_member_t *member, caller_control_action_t *action);
void conference_loop_floor_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_vid_floor_force(conference_member_t *member, caller_control_action_t *action);
void conference_loop_vmute_off(conference_member_t *member, caller_control_action_t *action);
void conference_loop_conference_video_vmute_snap(conference_member_t *member, caller_control_action_t *action);
void conference_loop_conference_video_vmute_snapoff(conference_member_t *member, caller_control_action_t *action);
void conference_loop_vmute_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_vmute_on(conference_member_t *member, caller_control_action_t *action);
void conference_loop_moh_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_border(conference_member_t *member, caller_control_action_t *action);
void conference_loop_deafmute_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_hangup(conference_member_t *member, caller_control_action_t *action);
void conference_loop_transfer(conference_member_t *member, caller_control_action_t *action);
void conference_loop_mute_off(conference_member_t *member, caller_control_action_t *action);
void conference_loop_event(conference_member_t *member, caller_control_action_t *action);
void conference_loop_transfer(conference_member_t *member, caller_control_action_t *action);
void conference_loop_exec_app(conference_member_t *member, caller_control_action_t *action);
void conference_loop_deaf_toggle(conference_member_t *member, caller_control_action_t *action);
void conference_loop_deaf_on(conference_member_t *member, caller_control_action_t *action);
void conference_loop_deaf_off(conference_member_t *member, caller_control_action_t *action);
void conference_set_variable(conference_obj_t *conference, const char *var, const char *val);
const char *conference_get_variable(conference_obj_t *conference, const char *var);


/* Global Structs */


/* API Interface Function sub-commands */
/* Entries in this list should be kept in sync with the enum above */
extern api_command_t conference_api_sub_commands[];
extern struct _mapping control_mappings[];
#define stream_write(__stream, __fmt, ...) if (__stream)__stream->write_function(__stream, __fmt, __VA_ARGS__)
#define VA_NONE "%s", ""
#endif /* MOD_CONFERENCE_H */

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
