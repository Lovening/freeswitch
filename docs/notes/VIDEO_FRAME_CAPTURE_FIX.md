# mod_rtpforward 视频帧捕获问题修复说明

## 问题描述

在 `mod_rtpforward` 中使用 `SWITCH_ABC_TYPE_STREAM_VIDEO_PING` 无法获取到 H.264 的 I 帧（关键帧）。

## 根本原因

### 1. 事件触发机制不同

FreeSWITCH 中有两种视频帧捕获事件：

- **`SWITCH_ABC_TYPE_READ_VIDEO_PING`**
  - 在 `switch_core_session_read_video_frame()` 函数中同步触发
  - 直接处理从 RTP 接收到的原始编码数据（H.264 NAL units）
  - **可以访问到完整的 RTP 包，包括 I 帧**
  - 触发时机：每次读取视频帧时

- **`SWITCH_ABC_TYPE_STREAM_VIDEO_PING`**
  - 在独立的 `video_bug_thread` 线程中触发
  - 只有设置了 `SMBF_READ_VIDEO_STREAM` 或 `SMBF_WRITE_VIDEO_STREAM` 标志才会创建该线程
  - 处理的是从队列中取出的**解码后的图像数据**（`switch_image_t`，通常是 I420 格式）
  - **此时 H.264 编码数据已经被解码成图像，无法直接访问 I 帧的编码数据**

### 2. 数据处理流程

```
原始 RTP 包 (H.264)
    ↓
switch_core_session_read_video_frame()
    ├─→ SWITCH_ABC_TYPE_READ_VIDEO_PING ✓ (可获取原始 H.264 数据)
    ↓
解码 (如果设置了 CF_VIDEO_DECODED_READ)
    ↓
switch_image_t (I420 等格式)
    ↓
放入 read_video_queue
    ↓
video_bug_thread 从队列取出
    ↓
SWITCH_ABC_TYPE_STREAM_VIDEO_PING ✗ (只能获取解码后的图像)
```

### 3. 标志配置问题

原代码使用的标志：
```c
SMBF_TAP_NATIVE_READ | SMBF_READ_VIDEO_PING | SMBF_NO_PAUSE | SMBF_ONE_ONLY
```

问题：
- `SMBF_TAP_NATIVE_READ` 用于音频原始数据捕获，对视频不适用
- 没有 `SMBF_READ_VIDEO_STREAM` 标志，所以 `video_bug_thread` 不会创建
- 结果：`SWITCH_ABC_TYPE_STREAM_VIDEO_PING` 事件永远不会被触发

## 修复方案

### 方案 1：使用 `SWITCH_ABC_TYPE_READ_VIDEO_PING`（已实施）

**优点：**
- 直接获取原始 H.264 编码数据，包括 I 帧
- 更低延迟（同步处理）
- 更简单，不需要额外的线程和队列
- 可以直接转发 RTP 包，无需重新编码

**修改要点：**

1. **Media bug 标志**：
```c
SMBF_READ_VIDEO_PING | SMBF_NO_PAUSE | SMBF_ONE_ONLY
```

2. **回调处理**：
```c
case SWITCH_ABC_TYPE_READ_VIDEO_PING:
    switch_frame_t* videoframe = switch_core_media_bug_get_video_ping_frame(bug);
    
    // 检查是否为关键帧
    if (switch_test_flag(videoframe, SFF_IS_KEYFRAME)) {
        // 这是 I 帧
    }
    
    // 发送完整的 RTP 包（包含 RTP 头）
    sendto(sock, videoframe->packet, videoframe->packetlen, ...);
```

3. **关键帧检测**：
```c
if (switch_test_flag(videoframe, SFF_IS_KEYFRAME)) {
    // I-frame detected
}
```

### 方案 2：使用 `SWITCH_ABC_TYPE_STREAM_VIDEO_PING`（需要重新编码）

如果确实需要使用 `STREAM_VIDEO_PING`（例如需要对图像进行处理），必须：

1. **添加必要的标志**：
```c
SMBF_READ_VIDEO_STREAM | SMBF_NO_PAUSE | SMBF_ONE_ONLY
```

2. **处理解码后的图像**：
```c
case SWITCH_ABC_TYPE_STREAM_VIDEO_PING:
    switch_frame_t* frame = switch_core_media_bug_get_video_ping_frame(bug);
    
    if (frame && frame->img) {
        // frame->img 是解码后的图像 (I420)
        // 需要重新编码为 H.264
        switch_core_codec_encode_video(&encoder, frame);
    }
```

3. **缺点**：
   - 需要重新编码（CPU 开销大）
   - 会有质量损失
   - 延迟更高
   - 更复杂的代码

## 测试验证

修复后，日志中应该能看到：

```
[I-FRAME] codec=H264, ssrc=12345, seq=100, ts=90000, len=5000, marker=1
[P-FRAME] codec=H264, ssrc=12345, seq=101, ts=93600, len=1200, marker=0
[P-FRAME] codec=H264, ssrc=12345, seq=102, ts=97200, len=1100, marker=0
```

## 相关代码位置

- `src/switch_core_media.c:15009-15220` - `switch_core_session_read_video_frame()`
- `src/switch_core_media_bug.c:592-760` - `video_bug_thread()`
- `src/switch_core_media_bug.c:730-750` - `SWITCH_ABC_TYPE_STREAM_VIDEO_PING` 触发点
- `src/switch_core_media.c:15180-15193` - `SWITCH_ABC_TYPE_READ_VIDEO_PING` 触发点

## 总结

- **`SWITCH_ABC_TYPE_READ_VIDEO_PING`** 适合：直接转发 RTP 流、录制、分析编码数据
- **`SWITCH_ABC_TYPE_STREAM_VIDEO_PING`** 适合：图像处理、视频混合、格式转换

对于 RTP 转发场景，使用 `READ_VIDEO_PING` 是正确的选择。
