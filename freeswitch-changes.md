# FreeSWITCH 修改记录

## 1. mod_vlc: VLC endpoint 加入会议不显示视频

**文件**: `src/mod/formats/mod_vlc/mod_vlc.c`

**问题**: VLC endpoint (`originate vlc/rtsp://...`) 不走 SDP 协商，`CF_VIDEO_READY` 未被设置，导致：
- 会议混流循环跳过该成员（不分配 layer）
- `members_with_video` 全局计数不含 VLC 成员
- 布局自动切换不触发

**修改** (`vlc_outgoing_channel`):
```c
switch_channel_mark_answered(channel);
/* VLC 不走 SDP 协商，需手动标记视频就绪，否则会议混流循环会跳过该成员 */
switch_channel_set_flag(channel, CF_VIDEO_READY);
```

---

## 2. mod_vlc: VLC 拉流不显示左下角叠加标签

**文件**: `src/mod/applications/mod_conference/conference_video_filter.c`

**问题**: VLC 拉流（rtsp 摄像头）加入会议后，视频左下角会显示 "Outbound Call" 文字和麦克风图标（来自 `caller_id_name` 变量）。

**修改** (`conference_video_init_layer_filter`):
```c
/* VLC 拉流成员（通道名以 vlc/ 开头）不叠加左下角标签，直接透传视频 */
if (channel) {
    const char *ch_name = switch_channel_get_name(channel);
    if (!zstr(ch_name) && !strncasecmp(ch_name, "vlc/", 4)) {
        return SWITCH_STATUS_FALSE;
    }
}
```

---

## 3. mod_conference: add-member 事件打印完整内容

**文件**: `src/mod/applications/mod_conference/conference_member.c`

**需求**: 排查 `originate vlc/rtsp://... &conference()` 是否触发 `add-member` 事件。

**修改** (事件发送前序列化并打印):
```c
{
    char *event_str = NULL;
    switch_event_serialize(event, &event_str, SWITCH_FALSE);
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
                      "Firing add-member event:\n%s\n", switch_str_nil(event_str));
    switch_safe_free(event_str);
}
switch_event_fire(&event);
```

---

## 4. mod_conference: canvas 0 没有 layout_group 导致不自动布局

**文件**: `src/mod/applications/mod_conference/conference_video.c`

**问题**: `video-canvas-count=2` 时，canvas 0 线程启动时 `conference->video_layout_group` 可能为 NULL（竞态），导致 `canvas->video_layout_group=NULL`，布局不自动切换。

**修改** (count_changed 块内补救):
```c
/* 若线程启动时存在竞态导致 layout_group 为 NULL，从 conference 补救 */
if (!canvas->video_layout_group && conference->video_layout_group) {
    canvas->video_layout_group = conference->video_layout_group;
}
```

---

## 5. mod_conference: 布局选择未计入音柱（avatar-only）成员

**文件**: `src/mod/applications/mod_conference/conference_video.c`

**问题**: 布局格子数选择只用 `video_count`（有视频的成员），忽略了只有头像（音柱等音频成员）的 avatar 成员。当 video(5) + avatar(6) = 11 > 9格(3x3)，后几个音柱拿不到 layer 不显示。

**修改**:
- 在 video_count 统计循环中同时统计 `avatar_count`（同画布、有 avatar_png_img、无 CF_VIDEO_READY 的成员）
- `count_changed` 检查纳入 `avatar_count` 变化
- 布局选择改为 `video_count + avatar_count - file_count`
- LAYOUT_DEBUG 日志增加 `avatar_count` 字段

---

## 会议配置 (temp-7005 profile)

```xml
<profile name="temp-7005">
  <param name="video-layout-name" value="group:grid"/>
  <param name="video-canvas-count" value="2"/>
  <param name="video-canvas-size" value="1280x720"/>
  <param name="video-canvas-bgcolor" value="#06172a"/>
  <param name="video-layout-bgcolor" value="#d4c4c4"/>
</profile>
```

布局组 `grid` 包含: 1x1, 2x1, 1x1+2x1, 2x2, 3x3, 4x4, 5x5, 6x6, 8x8

---

## 编译命令

```bash
make mod_vlc-install        # mod_vlc 变更后
make mod_conference-install # mod_conference 变更后
```

fs_cli 热加载:
```
reload mod_vlc
reload mod_conference
```

连接命令: `fs_cli -P 8121`



cd /home/user/workspace/freeswitch

# 查看哪些文件被修改了
git diff --stat

# 查看具体改动
git diff src/mod/formats/mod_vlc/mod_vlc.c
git diff src/mod/applications/mod_conference/conference_video_filter.c
git diff src/mod/applications/mod_conference/conference_member.c
git diff src/mod/applications/mod_conference/conference_video.c




# 只回退 mod_vlc
git checkout src/mod/formats/mod_vlc/mod_vlc.c

# 只回退 conference_video（布局相关）
git checkout src/mod/applications/mod_conference/conference_video.c

# 只回退 conference_video_filter（叠加标签）
git checkout src/mod/applications/mod_conference/conference_video_filter.c

# 只回退 conference_member（add-member 日志）
git checkout src/mod/applications/mod_conference/conference_member.c


git checkout src/mod/formats/mod_vlc/mod_vlc.c \
             src/mod/applications/mod_conference/conference_video.c \
             src/mod/applications/mod_conference/conference_video_filter.c \
             src/mod/applications/mod_conference/conference_member.c