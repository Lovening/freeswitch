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
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch
 * Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *
 * Anthony Minessale II <anthm@freeswitch.org>
 *
 *
 * switch_frame.h -- Media Frame Structure
 *
 */
/*! \file switch_frame.h
  \brief Media Frame Structure
*/

#ifndef SWITCH_FRAME_H
#define SWITCH_FRAME_H

#include <switch.h>

SWITCH_BEGIN_EXTERN_C

typedef struct switch_frame_geometry {
    int32_t w;
    int32_t h;
    int32_t x;
    int32_t y;
    int32_t z;
    int32_t M;
    int32_t X;
} switch_frame_geometry_t;

/*! \brief An abstraction of a data frame */
struct switch_frame {
    /*! a pointer to the codec information */
    // 编解码器信息指针 frame->codec->implementation->iananame 获取编解码器名称
    switch_codec_t* codec;
    /*! the originating source of the frame */
    // 帧来源
    const char* source;
    /*! the raw packet */
    // 原始数据包指针
    void* packet;
    /*! the size of the raw packet when applicable */
    // 原始数据包长度
    uint32_t packetlen;
    /*! the extra frame data */
    void* extra_data;
    /*! the frame data */
    // 帧数据指针 指向解码后的媒体数据
    void* data;
    /*! the size of the buffer that is in use */
    // 帧数据长度
    uint32_t datalen;
    /*! the entire size of the buffer */
    uint32_t buflen;  // 缓冲区总长度
    /*! the number of audio samples present (audio only) */
    uint32_t samples;  // 音频采样数量
    /*! the rate of the frame */
    uint32_t rate;  // 采样率
    /*! the number of channels in the frame */
    uint32_t channels;  // 声道数
    /*! the payload of the frame */
    switch_payload_t payload;  // RTP载荷类型  RTP包中的载荷类型标识 0=PCMU, 8=PCMA, 96=动态载荷
    /*! the timestamp of the frame */
    uint32_t timestamp;  // 时间戳 RTP时间戳，用于同步
    uint16_t seq;        // 序列号 RTP序列号，用于检测丢包和重排序
    uint32_t ssrc;
    switch_bool_t m;  // 标记位  RTP标记位，通常表示帧的开始或关键帧
    /*! frame flags */
    switch_frame_flag_t flags;  // 帧标志 SFF_IS_KEYFRAME: 关键帧 SFF_CNG: 舒适噪音 SFF_RTP_HEADER:
                                // 包含RTP头 SFF_PLC: 包丢失补偿
    void* user_data;            // 用户数据指针
    payload_map_t* pmap;        // 载荷映射 用途: RTP载荷类型映射信息
    switch_image_t* img;        // 视频图像指针
    struct switch_frame_geometry geometry;  // 几何信息
};

SWITCH_END_EXTERN_C
#endif
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
