# FreeSWITCH 异步队列+线程模式参考示例

本文档提供 FreeSWITCH 中使用异步队列和线程的完整示例，用于修改 mod_rtpforward。

## 核心概念

**问题**: 在 media bug 回调中直接调用 `sendto()` 会阻塞 RTP 媒体线程，导致视频马赛克。

**解决方案**: 
1. 在回调中只做**快速入队**操作（拷贝数据）
2. 独立的**发送线程**从队列取数据并调用 `sendto()`

---

## 示例 1: switch_log.c - 日志系统（最简单）

### 数据结构

```c
// 全局队列和线程
static switch_queue_t *LOG_QUEUE = NULL;
static switch_thread_t *thread = NULL;
static int THREAD_RUNNING = 0;

// 队列元素结构
typedef struct {
    uint8_t *data;
    size_t len;
} queued_packet_t;
```

### 初始化队列和线程

```c
SWITCH_DECLARE(switch_status_t) switch_log_init(switch_memory_pool_t *pool, switch_bool_t colorize)
{
    switch_threadattr_t *thd_attr;

    LOG_POOL = pool;

    // 1. 创建线程属性
    switch_threadattr_create(&thd_attr, LOG_POOL);
    
    // 2. 创建队列（容量：SWITCH_CORE_QUEUE_LEN = 100000）
    switch_queue_create(&LOG_QUEUE, SWITCH_CORE_QUEUE_LEN, LOG_POOL);
    
    // 3. 设置线程栈大小
    switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);
    
    // 4. 创建线程
    switch_thread_create(&thread, thd_attr, log_thread, NULL, LOG_POOL);

    // 5. 等待线程启动
    while (!THREAD_RUNNING) {
        switch_cond_next();
    }

    return SWITCH_STATUS_SUCCESS;
}
```

### 线程函数

```c
static void *SWITCH_THREAD_FUNC log_thread(switch_thread_t *t, void *obj)
{
    THREAD_RUNNING = 1;

    // 主循环：持续从队列取数据
    while (THREAD_RUNNING == 1) {
        void *pop = NULL;
        switch_log_node_t *node = NULL;

        // 阻塞等待队列数据（pop 会阻塞直到有数据）
        if (switch_queue_pop(LOG_QUEUE, &pop) != SWITCH_STATUS_SUCCESS) {
            break;
        }

        if (!pop) {
            THREAD_RUNNING = -1;
            break;
        }

        node = (switch_log_node_t *) pop;
        
        // 处理数据（写日志）
        for (binding = BINDINGS; binding; binding = binding->next) {
            if (binding->level >= node->level) {
                binding->function(node, node->level);
            }
        }

        // 释放内存
        switch_log_node_free(&node);
    }

    THREAD_RUNNING = 0;
    return NULL;
}
```

### 生产者：入队数据

```c
// 在快速路径中（如回调函数）
if (switch_queue_trypush(LOG_QUEUE, node) != SWITCH_STATUS_SUCCESS) {
    // 队列满，丢弃数据
    switch_log_node_free(&node);
}
```

### 关键API

| API | 用途 | 是否阻塞 |
|-----|------|---------|
| `switch_queue_create(&queue, size, pool)` | 创建队列 | 否 |
| `switch_queue_trypush(queue, data)` | 尝试入队（队列满则失败） | **否** ⚡ |
| `switch_queue_push(queue, data)` | 阻塞入队（队列满则等待） | 是 |
| `switch_queue_pop(queue, &data)` | 阻塞出队（队列空则等待） | 是 ⏳ |
| `switch_queue_trypop(queue, &data)` | 尝试出队（队列空则失败） | 否 |
| `switch_queue_pop_timeout(queue, &data, ms)` | 超时出队 | 是（超时） |

---

## 示例 2: switch_core_media_bug.c - 视频队列（推荐参考）

### 数据结构

```c
struct switch_media_bug {
    // 视频队列
    switch_queue_t *read_video_queue;
    switch_queue_t *write_video_queue;
    
    // 视频处理线程
    switch_thread_t *video_thread;
    
    // 其他字段...
};
```

### 队列初始化

```c
// 在 switch_core_media_bug_add 中
if (switch_test_flag(bug, SMBF_READ_VIDEO_STREAM)) {
    switch_queue_create(&bug->read_video_queue, SWITCH_CORE_QUEUE_LEN, pool);
}

if (switch_test_flag(bug, SMBF_WRITE_VIDEO_STREAM)) {
    switch_queue_create(&bug->write_video_queue, SWITCH_CORE_QUEUE_LEN, pool);
}
```

### 线程函数

```c
static void *SWITCH_THREAD_FUNC video_bug_thread(switch_thread_t *thread, void *obj)
{
    switch_media_bug_t *bug = (switch_media_bug_t *) obj;
    switch_queue_t *main_q = bug->read_video_queue;
    void *pop;
    switch_image_t *img = NULL;

    while (bug->ready) {
        // 超时出队（100ms）
        if (switch_queue_pop_timeout(main_q, &pop, 100000) == SWITCH_STATUS_SUCCESS) {
            if (!pop) break;  // NULL = 停止信号
            
            img = (switch_image_t *)pop;
            
            // 处理图像
            process_image(img);
            
            // 释放图像
            switch_img_free(&img);
        }
    }

    // 清空队列
    while (switch_queue_trypop(main_q, &pop) == SWITCH_STATUS_SUCCESS) {
        if (pop) {
            img = (switch_image_t *)pop;
            switch_img_free(&img);
        }
    }

    return NULL;
}
```

### 入队操作

```c
// 在媒体回调中
if (frame && frame->img) {
    switch_image_t *img_copy = NULL;
    
    // 拷贝图像数据
    switch_img_copy(frame->img, &img_copy);
    
    // 非阻塞入队
    if (switch_queue_trypush(bug->read_video_queue, img_copy) != SWITCH_STATUS_SUCCESS) {
        // 队列满，释放拷贝的图像
        switch_img_free(&img_copy);
    }
}
```

---

## 应用到 mod_rtpforward

### 1. 修改上下文结构

```c
typedef struct {
    uint8_t *data;
    size_t len;
} queued_packet_t;

typedef struct rtpforward_context {
    // 现有字段...
    
    // 新增：异步发送队列和线程
    switch_queue_t *audio_queue;
    switch_queue_t *video_queue;
    switch_thread_t *audio_send_thread;
    switch_thread_t *video_send_thread;
    
    // 其他字段...
} rtpforward_context_t;
```

### 2. 发送线程函数

```c
/* 音频发送线程 */
static void *SWITCH_THREAD_FUNC audio_send_thread_run(switch_thread_t *thread, void *obj)
{
    rtpforward_context_t *context = (rtpforward_context_t *)obj;
    void *pop = NULL;
    queued_packet_t *pkt = NULL;
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                      "rtpforward: audio send thread started\\n");
    
    while (context->running) {
        // 100ms 超时出队
        if (switch_queue_pop_timeout(context->audio_queue, &pop, 100000) == SWITCH_STATUS_SUCCESS) {
            pkt = (queued_packet_t *)pop;
            if (pkt && pkt->data && pkt->len > 0) {
                // 发送 RTP 包（可以阻塞，不影响媒体线程）
                sendto(context->audio_sock, pkt->data, pkt->len, 0,
                       (struct sockaddr *)&context->audio_addr, 
                       sizeof(context->audio_addr));
                       
                free(pkt->data);
            }
            free(pkt);
        }
    }
    
    // 清空队列
    while (switch_queue_trypop(context->audio_queue, &pop) == SWITCH_STATUS_SUCCESS) {
        pkt = (queued_packet_t *)pop;
        if (pkt) {
            free(pkt->data);
            free(pkt);
        }
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                      "rtpforward: audio send thread stopped\\n");
    return NULL;
}

/* 视频发送线程（类似） */
static void *SWITCH_THREAD_FUNC video_send_thread_run(switch_thread_t *thread, void *obj)
{
    rtpforward_context_t *context = (rtpforward_context_t *)obj;
    void *pop = NULL;
    queued_packet_t *pkt = NULL;
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                      "rtpforward: video send thread started\\n");
    
    while (context->running) {
        if (switch_queue_pop_timeout(context->video_queue, &pop, 100000) == SWITCH_STATUS_SUCCESS) {
            pkt = (queued_packet_t *)pop;
            if (pkt && pkt->data && pkt->len > 0) {
                sendto(context->video_sock, pkt->data, pkt->len, 0,
                       (struct sockaddr *)&context->video_addr, 
                       sizeof(context->video_addr));
                free(pkt->data);
            }
            free(pkt);
        }
    }
    
    // 清空队列
    while (switch_queue_trypop(context->video_queue, &pop) == SWITCH_STATUS_SUCCESS) {
        pkt = (queued_packet_t *)pop;
        if (pkt) {
            free(pkt->data);
            free(pkt);
        }
    }
    
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                      "rtpforward: video send thread stopped\\n");
    return NULL;
}
```

### 3. 初始化（在 rtpforward_start_function 中）

```c
// 在创建 socket 后
context->running = SWITCH_TRUE;

// 创建队列（容量 500 个包）
if (switch_queue_create(&context->audio_queue, 500, context->pool) != SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "-ERR Failed to create audio queue\\n");
    goto done;
}

if (switch_queue_create(&context->video_queue, 500, context->pool) != SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "-ERR Failed to create video queue\\n");
    goto done;
}

// 创建线程
switch_threadattr_t *thd_attr = NULL;
switch_threadattr_create(&thd_attr, context->pool);
switch_threadattr_detach_set(thd_attr, 0);  // 不分离，可以 join
switch_threadattr_stacksize_set(thd_attr, SWITCH_THREAD_STACKSIZE);

if (switch_thread_create(&context->audio_send_thread, thd_attr, 
                         audio_send_thread_run, context, context->pool) != SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "-ERR Failed to create audio send thread\\n");
    goto done;
}

if (switch_thread_create(&context->video_send_thread, thd_attr,
                         video_send_thread_run, context, context->pool) != SWITCH_STATUS_SUCCESS) {
    stream->write_function(stream, "-ERR Failed to create video send thread\\n");
    goto done;
}

switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_CONSOLE,
                  "rtpforward: async send threads created\\n");
```

### 4. 修改回调函数（audio_bug_callback）

```c
case SWITCH_ABC_TYPE_TAP_NATIVE_READ:
    if (frame->packet && frame->packetlen > 0) {
        queued_packet_t *pkt = malloc(sizeof(queued_packet_t));
        if (pkt) {
            // 拷贝 RTP 包数据
            pkt->data = malloc(frame->packetlen);
            if (pkt->data) {
                memcpy(pkt->data, frame->packet, frame->packetlen);
                pkt->len = frame->packetlen;
                
                // 非阻塞入队
                if (switch_queue_trypush(context->audio_queue, pkt) != SWITCH_STATUS_SUCCESS) {
                    // 队列满，丢弃包
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                      "rtpforward: audio queue full, dropping packet\\n");
                    free(pkt->data);
                    free(pkt);
                }
            } else {
                free(pkt);
            }
        }
    }
    break;
```

### 5. 修改回调函数（video_bug_callback）

```c
case SWITCH_ABC_TYPE_READ_VIDEO_RAW:
    if (context->running && context->video_sock >= 0) {
        switch_frame_t *vf = switch_core_media_bug_get_video_ping_frame(bug);
        if (vf && vf->packet && vf->packetlen > 12) {
            queued_packet_t *pkt = malloc(sizeof(queued_packet_t));
            if (pkt) {
                pkt->data = malloc(vf->packetlen);
                if (pkt->data) {
                    memcpy(pkt->data, vf->packet, vf->packetlen);
                    pkt->len = vf->packetlen;
                    
                    if (switch_queue_trypush(context->video_queue, pkt) != SWITCH_STATUS_SUCCESS) {
                        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
                                          "rtpforward: video queue full, dropping packet\\n");
                        free(pkt->data);
                        free(pkt);
                    }
                } else {
                    free(pkt);
                }
            }
        }
    }
    break;
```

### 6. 清理（在 destroy_forward_context 中）

```c
static void destroy_forward_context(rtpforward_context_t *context)
{
    if (!context) return;
    
    // 幂等检查
    switch_mutex_lock(context->mutex);
    if (context->destroying) {
        switch_mutex_unlock(context->mutex);
        return;
    }
    context->destroying = 1;
    switch_mutex_unlock(context->mutex);

    // 停止线程标志
    context->running = SWITCH_FALSE;
    
    // 等待发送线程结束
    if (context->audio_send_thread) {
        switch_status_t st;
        switch_thread_join(&st, context->audio_send_thread);
        context->audio_send_thread = NULL;
    }
    
    if (context->video_send_thread) {
        switch_status_t st;
        switch_thread_join(&st, context->video_send_thread);
        context->video_send_thread = NULL;
    }
    
    // 关闭套接字
    if (context->audio_sock >= 0) {
        close(context->audio_sock);
        context->audio_sock = -1;
    }
    
    if (context->video_sock >= 0) {
        close(context->video_sock);
        context->video_sock = -1;
    }
    
    // 移除 media bugs
    if (context->audio_bug) {
        switch_core_media_bug_remove(context->session, &context->audio_bug);
        context->audio_bug = NULL;
    }
    
    if (context->video_bug) {
        switch_core_media_bug_remove(context->session, &context->video_bug);
        context->video_bug = NULL;
    }
    
    // 解绑事件
    if (context->event_node) {
        switch_event_unbind(&context->event_node);
        context->event_node = NULL;
    }
}
```

---

## 性能优化建议

### 队列大小选择

| 场景 | 推荐大小 | 说明 |
|------|---------|------|
| 音频（8kHz, 20ms ptime） | 500 | 50 包/秒，10秒缓冲 |
| 视频（720p30, ~10包/帧） | 500-1000 | 300 包/秒，1.6-3.3秒缓冲 |
| 视频（1080p30, ~20包/帧） | 1000-2000 | 600 包/秒，1.6-3.3秒缓冲 |

### 内存管理

```c
// 推荐：使用内存池（自动管理）
pkt->data = switch_core_alloc(context->pool, frame->packetlen);

// 当前方案：使用 malloc（需要手动 free）
pkt->data = malloc(frame->packetlen);
// ... 使用后
free(pkt->data);
```

### 监控队列状态

```c
// 可选：添加统计信息
typedef struct {
    uint32_t enqueue_count;
    uint32_t dequeue_count;
    uint32_t drop_count;
    uint32_t max_size;
} queue_stats_t;
```

---

## 关键要点

1. ✅ **回调中只做拷贝+入队**（非阻塞，<1μs）
2. ✅ **发送线程可以阻塞**（不影响媒体线程）
3. ✅ **使用 `switch_queue_trypush`** 避免阻塞
4. ✅ **队列满时丢弃包**（不能让回调阻塞）
5. ✅ **停止时清空队列**（避免内存泄漏）
6. ✅ **使用 `switch_thread_join`** 等待线程结束

---

## 测试验证

### 验证异步生效

```bash
# 查看线程是否创建
freeswitch@internal> rtpforward_start <uuid> <ip> <audio_port> <video_port>
# 应该看到日志：
# rtpforward: audio send thread started
# rtpforward: video send thread started

# 查看系统线程
ps -eLf | grep freeswitch | wc -l
```

### 验证无阻塞

```bash
# 使用 tc 模拟网络延迟
sudo tc qdisc add dev eth0 root netem delay 100ms

# 视频应该流畅，不再马赛克
```

### 验证队列统计

```c
// 在 rtpforward_list 中添加
stream->write_function(stream, "  Audio Queue: %d/%d\\n",
                       switch_queue_size(ctx->audio_queue), 500);
stream->write_function(stream, "  Video Queue: %d/%d\\n",
                       switch_queue_size(ctx->video_queue), 500);
```

---

## 参考文件

1. **日志系统**: `src/switch_log.c:488-530,755-765`
2. **媒体 bug**: `src/switch_core_media_bug.c:592-650,932-952`
3. **本地流**: `src/mod/formats/mod_local_stream/mod_local_stream.c`
4. **MSRP**: `src/switch_msrp.c:1051-1100,1590`

---

## 编译和测试

```bash
cd /home/ning/workSpace/freeswitch
make mod_rtpforward-install
fs_cli -x "reload mod_rtpforward"
```
