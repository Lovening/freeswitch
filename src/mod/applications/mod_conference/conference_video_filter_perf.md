# conference_video_filter.c 性能优化建议

## 问题一（严重）：每帧 `av_frame_get_buffer` 触发 malloc

### 现状

`conference_video_apply_layer_filter` 中每帧执行：

```c
ret = av_frame_get_buffer(layer->filter_frame_in, 32);
// ... 使用 ...
av_frame_unref(layer->filter_frame_in);
```

720p（1280×720）YUV420P 每帧约 1.4 MB，30fps 时每秒触发 30 次 `malloc/free`。

### 根因

使用了 `AV_BUFFERSRC_FLAG_KEEP_REF`，FFmpeg 持有引用导致无法复用内存。

### 修改方法

**步骤一**：在 `mcu_layer_t` 结构体中（`mod_conference.h`）添加预分配标志：

```c
switch_bool_t filter_frame_in_allocated;  /* 输入帧缓冲区是否已预分配 */
```

**步骤二**：在 `conference_video_init_layer_filter` 分配 AVFrame 之后、设置 `filter_enabled` 之前预分配缓冲区：

```c
/* 预分配输入帧缓冲区，后续每帧复用 */
layer->filter_frame_in->format = AV_PIX_FMT_YUV420P;
layer->filter_frame_in->width  = width;
layer->filter_frame_in->height = height;
ret = av_frame_get_buffer(layer->filter_frame_in, 32);
if (ret < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "Failed to pre-allocate AVFrame buffer for member %d\n", member->id);
    goto error;
}
layer->filter_frame_in_allocated = SWITCH_TRUE;
```

**步骤三**：在 `conference_video_apply_layer_filter` 中改为 `av_frame_make_writable`：

```c
/* 删除：
 * layer->filter_frame_in->format = AV_PIX_FMT_YUV420P;
 * layer->filter_frame_in->width  = (*img)->d_w;
 * layer->filter_frame_in->height = (*img)->d_h;
 * layer->filter_frame_in->pts = AV_NOPTS_VALUE;
 * ret = av_frame_get_buffer(layer->filter_frame_in, 32);
 */

/* 新增：确保帧可写，引用计数为1时零开销 */
layer->filter_frame_in->pts = AV_NOPTS_VALUE;
ret = av_frame_make_writable(layer->filter_frame_in);
if (ret < 0) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "Failed to make AVFrame writable: %d\n", ret);
    return SWITCH_STATUS_FALSE;
}
```

**步骤四**：去掉 `AV_BUFFERSRC_FLAG_KEEP_REF` 并移除 `av_frame_unref`：

```c
/* 修改前 */
ret = av_buffersrc_add_frame_flags(layer->buffersrc_ctx, layer->filter_frame_in,
    AV_BUFFERSRC_FLAG_KEEP_REF);
av_frame_unref(layer->filter_frame_in);

/* 修改后：不带 KEEP_REF，FFmpeg 内部复制数据，帧缓冲区保留复用 */
ret = av_buffersrc_add_frame_flags(layer->buffersrc_ctx, layer->filter_frame_in, 0);
/* 不调用 av_frame_unref */
```

**步骤五**：在 `conference_video_destroy_layer_filter` 中清除标志：

```c
if (layer->filter_frame_in) {
    av_frame_free(&layer->filter_frame_in);
    layer->filter_frame_in = NULL;
}
layer->filter_frame_in_allocated = SWITCH_FALSE;
```

---

## 问题二（严重）：每帧 `switch_img_alloc` / `switch_img_free`

### 现状

```c
new_img = switch_img_alloc(NULL, SWITCH_IMG_FMT_I420,
    layer->filter_frame_out->width, layer->filter_frame_out->height, 1);
// ... 复制数据 ...
switch_img_free(img);
*img = new_img;
```

每帧申请约 1.4 MB 输出图像，帧结束后释放。

### 修改方法

**步骤一**：在 `mcu_layer_t` 结构体中添加预分配输出图像字段：

```c
switch_image_t *filter_img_out;  /* 预分配的输出图像，复用 */
```

**步骤二**：在 `conference_video_init_layer_filter` 中预分配：

```c
if (layer->filter_img_out) {
    switch_img_free(&layer->filter_img_out);
}
layer->filter_img_out = switch_img_alloc(NULL, SWITCH_IMG_FMT_I420, width, height, 1);
if (!layer->filter_img_out) {
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
        "Failed to pre-allocate output image for member %d\n", member->id);
    goto error;
}
```

**步骤三**：在 `conference_video_apply_layer_filter` 中复用，交换指针：

```c
/* 删除：new_img = switch_img_alloc(...); */

/* 直接向 layer->filter_img_out 写入数据（复制三个平面的代码不变，目标改为 layer->filter_img_out） */

/* 原来：
 * switch_img_free(img);
 * *img = new_img;
 */
/* 修改为：交换指针，原帧留作下次输出缓冲区 */
{
    switch_image_t *tmp = *img;
    *img = layer->filter_img_out;
    layer->filter_img_out = tmp;
}
```

> **注意**：交换前提是调用方在取得 `*img` 后不会二次释放同一指针。若生命周期不确定，可退而求其次：仅复用输出 buffer、保留 `switch_img_free`，只取消每帧的 `switch_img_alloc`。

**步骤四**：在 `conference_video_destroy_layer_filter` 中释放：

```c
if (layer->filter_img_out) {
    switch_img_free(&layer->filter_img_out);
    layer->filter_img_out = NULL;
}
```

---

## 问题三（中等）：逐行 memcpy 未合并

### 现状

```c
for (i = 0; i < (*img)->d_h; i++) {
    memcpy(dst + i * dst_linesize, src + i * src_stride, width);
}
```

720p 时 Y 平面 720 次、U/V 各 360 次，合计每帧 1440 次 `memcpy` 调用。

### 修改方法

检测 stride 是否连续，连续时合并为单次调用：

```c
/* Y 平面 */
if ((*img)->stride[SWITCH_PLANE_Y] == (*img)->d_w &&
    layer->filter_frame_in->linesize[0] == (*img)->d_w) {
    memcpy(layer->filter_frame_in->data[0],
           (*img)->planes[SWITCH_PLANE_Y],
           (size_t)(*img)->d_w * (*img)->d_h);
} else {
    for (i = 0; i < (*img)->d_h; i++) {
        memcpy(layer->filter_frame_in->data[0] + i * layer->filter_frame_in->linesize[0],
               (*img)->planes[SWITCH_PLANE_Y] + i * (*img)->stride[SWITCH_PLANE_Y],
               (*img)->d_w);
    }
}

/* U 平面（chroma_w = d_w/2, chroma_h = d_h/2） */
if ((*img)->stride[SWITCH_PLANE_U] == (*img)->d_w / 2 &&
    layer->filter_frame_in->linesize[1] == (*img)->d_w / 2) {
    memcpy(layer->filter_frame_in->data[1],
           (*img)->planes[SWITCH_PLANE_U],
           (size_t)((*img)->d_w / 2) * ((*img)->d_h / 2));
} else {
    for (i = 0; i < (*img)->d_h / 2; i++) {
        memcpy(layer->filter_frame_in->data[1] + i * layer->filter_frame_in->linesize[1],
               (*img)->planes[SWITCH_PLANE_U] + i * (*img)->stride[SWITCH_PLANE_U],
               (*img)->d_w / 2);
    }
}

/* V 平面（同 U） */
```

对输出方向（`filter_frame_out` → `filter_img_out`）做相同处理。

---

## 问题四（轻微）：`switch_file_exists` 重复 stat 调用

### 现状

每次 init（尺寸变化、静音变化）均调用一次 `stat()` 系统调用。

### 修改方法

在 `mcu_layer_t` 中缓存检测结果，仅当图标路径变化时才重新检测：

```c
/* 新增字段 */
switch_bool_t filter_mic_icon_available;
char          filter_mic_image_path[256];
```

```c
/* init 函数中替换 switch_file_exists 调用 */
if (strncmp(layer->filter_mic_image_path, mic_image_path,
            sizeof(layer->filter_mic_image_path) - 1) != 0) {
    layer->filter_mic_icon_available =
        (switch_file_exists(mic_image_path, NULL) == SWITCH_STATUS_SUCCESS)
        ? SWITCH_TRUE : SWITCH_FALSE;
    switch_snprintf(layer->filter_mic_image_path,
                    sizeof(layer->filter_mic_image_path), "%s", mic_image_path);
}

if (layer->filter_mic_icon_available) {
    /* 图标版 filter_desc */
} else {
    /* 纯文字 filter_desc */
}
```

---

## 优化优先级汇总

| 优先级 | 问题 | 触发频率 | 优化收益 |
|--------|------|---------|---------|
| P0 | 每帧 `av_frame_get_buffer` malloc | 每帧（30fps+） | 消除每帧 ~1.4MB malloc，降低堆碎片和 CPU 开销 |
| P0 | 每帧 `switch_img_alloc` malloc | 每帧（30fps+） | 同上 |
| P1 | 逐行 memcpy 未合并 | 每帧 | 减少函数调用 1440 → 最少 3 次 |
| P2 | `switch_file_exists` 重复 stat | 静音/尺寸变化时 | 减少系统调用，影响较小 |
