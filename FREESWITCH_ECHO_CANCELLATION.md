# FreeSWITCH 回声处理机制

## 概述

FreeSWITCH 使用 **Speex DSP 库** 实现回声消除（AEC - Acoustic Echo Cancellation），这是一种基于自适应滤波器的声学回声消除技术。

### 关键特性

1. **双向回声消除**：同时对读（接收）和写（发送）方向进行处理
2. **Media Bug 机制**：通过 media bug 钩子拦截音频流进行处理
3. **预处理流水线**：回声消除 → 噪声抑制 → 自动增益控制（AGC）

---

## 核心数据结构

位置：`src/switch_ivr_async.c:3430-3442`

```c
typedef struct {
    SpeexPreprocessState* read_st;   // 读方向预处理状态
    SpeexPreprocessState* write_st;  // 写方向预处理状态
    SpeexEchoState* read_ec;         // 读方向回声消除状态
    SpeexEchoState* write_ec;        // 写方向回声消除状态
    switch_byte_t read_data[2048];   // 存储读方向的音频（作为参考信号）
    switch_byte_t write_data[2048];  // 存储写方向的音频（作为参考信号）
    switch_byte_t read_out[2048];    // 回声消除后的读输出
    switch_byte_t write_out[2048];   // 回声消除后的写输出
    switch_mutex_t* read_mutex;
    switch_mutex_t* write_mutex;
    int done;
} pp_cb_t;
```

### 字段说明

| 字段 | 类型 | 说明 |
|------|------|------|
| `read_st` | `SpeexPreprocessState*` | 读方向的 Speex 预处理状态（含噪声抑制、AGC） |
| `write_st` | `SpeexPreprocessState*` | 写方向的 Speex 预处理状态 |
| `read_ec` | `SpeexEchoState*` | 读方向的回声消除器状态 |
| `write_ec` | `SpeexEchoState*` | 写方向的回声消除器状态 |
| `read_data` | `switch_byte_t[2048]` | 缓存读方向音频，作为写方向回声消除的参考信号 |
| `write_data` | `switch_byte_t[2048]` | 缓存写方向音频，作为读方向回声消除的参考信号 |
| `read_out` | `switch_byte_t[2048]` | 读方向回声消除后的输出缓冲区 |
| `write_out` | `switch_byte_t[2048]` | 写方向回声消除后的输出缓冲区 |

---

## 回声消除工作流程

```
┌─────────────────────────────────────────────────────────────┐
│                      Media Bug 回调                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   READ_REPLACE (读取远端音频)                                │
│   ┌─────────────────────────────────────────────────────┐   │
│   │  1. 获取读取帧 (frame)                               │   │
│   │  2. speex_echo_cancellation(                         │   │
│   │        read_ec,      // 回声消除器状态               │   │
│   │        frame->data,  // 当前麦克风输入（近端信号）    │   │
│   │        write_data,   // 参考信号(之前播放的音频)      │   │
│   │        read_out)     // 消除回声后的输出             │   │
│   │  3. memcpy(frame->data, read_out, ...)              │   │
│   │  4. speex_preprocess_run(read_st, frame->data)      │   │
│   │  5. 保存当前读取音频到 read_data (供写方向参考)      │   │
│   └─────────────────────────────────────────────────────┘   │
│                                                             │
│   WRITE_REPLACE (发送近端音频)                               │
│   ┌─────────────────────────────────────────────────────┐   │
│   │  1. 获取写入帧 (frame)                               │   │
│   │  2. speex_echo_cancellation(                         │   │
│   │        write_ec,     // 回声消除器状态               │   │
│   │        frame->data,  // 当前要发送的音频             │   │
│   │        read_data,    // 参考信号(之前接收的音频)      │   │
│   │        write_out)    // 消除回声后的输出             │   │
│   │  3. memcpy(frame->data, write_out, ...)             │   │
│   │  4. speex_preprocess_run(write_st, frame->data)     │   │
│   │  5. 保存当前写入音频到 write_data (供读方向参考)     │   │
│   └─────────────────────────────────────────────────────┘   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 回调函数处理逻辑

位置：`src/switch_ivr_async.c:3444-3521`

```c
static switch_bool_t preprocess_callback(switch_media_bug_t* bug, void* user_data,
                                         switch_abc_type_t type) {
    pp_cb_t* cb = (pp_cb_t*)user_data;

    switch (type) {
        case SWITCH_ABC_TYPE_INIT:
            // 初始化互斥锁
            break;

        case SWITCH_ABC_TYPE_CLOSE:
            // 销毁所有 Speex 状态
            if (cb->read_st) speex_preprocess_state_destroy(cb->read_st);
            if (cb->write_st) speex_preprocess_state_destroy(cb->write_st);
            if (cb->read_ec) speex_echo_state_destroy(cb->read_ec);
            if (cb->write_ec) speex_echo_state_destroy(cb->write_ec);
            break;

        case SWITCH_ABC_TYPE_READ_REPLACE:
            // 读方向处理：使用 write_data 作为参考信号消除回声
            if (cb->read_ec) {
                speex_echo_cancellation(cb->read_ec, frame->data,
                                        cb->write_data, cb->read_out);
                memcpy(frame->data, cb->read_out, frame->datalen);
            }
            speex_preprocess_run(cb->read_st, frame->data);

            // 保存读数据供写方向使用
            if (cb->write_ec) {
                memcpy(cb->read_data, frame->data, frame->datalen);
            }
            break;

        case SWITCH_ABC_TYPE_WRITE_REPLACE:
            // 写方向处理：使用 read_data 作为参考信号消除回声
            if (cb->write_ec) {
                speex_echo_cancellation(cb->write_ec, frame->data,
                                        cb->read_data, cb->write_out);
                memcpy(frame->data, cb->write_out, frame->datalen);
            }
            speex_preprocess_run(cb->write_st, frame->data);

            // 保存写数据供读方向使用
            if (cb->read_ec) {
                memcpy(cb->write_data, frame->data, frame->datalen);
            }
            break;
    }
    return SWITCH_TRUE;
}
```

---

## 回声消除初始化

位置：`src/switch_ivr_async.c:3624-3658`

```c
// echo_cancel 参数格式: r.echo_cancel=tail 或 w.echo_cancel=tail
// tail 是回声尾长（默认 1024 样本），决定了滤波器长度
if (!strcasecmp(var, "echo_cancel")) {
    int tail = 1024;  // 默认回声尾长
    int tmp = atoi(val);

    if (!tr && tmp > 0) {
        tail = tmp;
    }

    // 销毁旧的回声消除器（如果存在）
    if (ec) {
        if (rw == 'r') {
            speex_echo_state_destroy(cb->read_ec);
            cb->read_ec = NULL;
        } else {
            speex_echo_state_destroy(cb->write_ec);
            cb->write_ec = NULL;
        }
        ec = NULL;
    }

    // 初始化新的回声消除状态
    if (!ec) {
        if (rw == 'r') {
            ec = cb->read_ec = speex_echo_state_init(
                read_impl.samples_per_packet,  // 帧大小
                tail                            // 回声尾长
            );
            speex_echo_ctl(ec, SPEEX_ECHO_SET_SAMPLING_RATE,
                           &read_impl.samples_per_second);
            flags |= SMBF_WRITE_REPLACE;  // 需要写方向数据作为参考
        } else {
            ec = cb->write_ec = speex_echo_state_init(
                read_impl.samples_per_packet,
                tail
            );
            speex_echo_ctl(ec, SPEEX_ECHO_SET_SAMPLING_RATE,
                           &read_impl.samples_per_second);
            flags |= SMBF_READ_REPLACE;  // 需要读方向数据作为参考
        }

        // 将回声消除器与预处理器关联
        speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_ECHO_STATE, ec);
    }
}
```

---

## 使用方式

### Dialplan 中启用

```xml
<!-- 启用读方向回声消除 -->
<action application="preprocess" data="r.echo_cancel=1024"/>

<!-- 完整配置：回声消除 + 噪声抑制 + AGC -->
<action application="preprocess" data="r.echo_cancel=1024,r.noise_suppress=-30,r.agc=8000"/>
```

### Lua 脚本中调用

```lua
-- 基本回声消除
session:execute("preprocess", "r.echo_cancel=1024")

-- 完整配置
session:execute("preprocess", "r.echo_cancel=1024,r.noise_suppress=-30,r.agc=8000")

-- 双向处理
session:execute("preprocess", "r.echo_cancel=1024,w.echo_cancel=1024")

-- 停止预处理
session:execute("preprocess", "stop")
```

### ESL 接口调用

```python
# Python ESL 示例
conn.api("uuid_preprocess <uuid> r.echo_cancel=1024")
```

---

## 可配置参数

| 参数 | 方向 | 说明 | 示例值 |
|------|------|------|--------|
| `echo_cancel` | r/w | 启用回声消除，设置回声尾长（样本数） | `1024`, `2048` |
| `echo_suppress` | r/w | 回声抑制级别（负 dB 值） | `-40`, `-50` |
| `noise_suppress` | r/w | 噪声抑制级别（负 dB 值） | `-30`, `-40` |
| `agc` | r/w | 自动增益控制，可设置目标电平 | `true`, `8000` |

### 参数格式

```
<direction>.<parameter>=<value>[,<direction>.<parameter>=<value>...]
```

- `direction`: `r` (读方向) 或 `w` (写方向)
- 多个参数用逗号分隔

### 示例配置

```xml
<!-- 会议场景：强回声消除 + 噪声抑制 -->
<action application="preprocess" data="r.echo_cancel=2048,r.noise_suppress=-40,r.agc=true"/>

<!-- 录音场景：仅噪声抑制 -->
<action application="preprocess" data="r.noise_suppress=-30"/>

<!-- 双向全功能 -->
<action application="preprocess" data="r.echo_cancel=1024,r.noise_suppress=-30,r.agc=8000,w.echo_cancel=1024"/>
```

---

## 回声尾长 (Tail Length) 选择

回声尾长决定了自适应滤波器的长度，直接影响回声消除效果。

| 采样率 | 256 样本 | 512 样本 | 1024 样本 | 2048 样本 |
|--------|----------|----------|-----------|-----------|
| 8 kHz  | 32 ms    | 64 ms    | 128 ms    | 256 ms    |
| 16 kHz | 16 ms    | 32 ms    | 64 ms     | 128 ms    |
| 32 kHz | 8 ms     | 16 ms    | 32 ms     | 64 ms     |
| 48 kHz | 5.3 ms   | 10.7 ms  | 21.3 ms   | 42.7 ms   |

### 选择建议

- **太短**：无法消除长延迟的回声
- **太长**：增加计算复杂度和收敛时间
- **典型值**：256-2048 样本
- **会议场景**：推荐 1024-2048
- **普通通话**：推荐 512-1024

---

## 算法原理

FreeSWITCH 使用 Speex AEC 的自适应滤波器算法（基于 NLMS/MDF），工作原理：

```
回声路径模型：
远端信号 ──→ [扬声器] ──→ [房间回声路径 H(z)] ──→ [麦克风] ──→ 混合信号
                              │
                              ↓
         自适应滤波器 Ĥ(z) 试图逼近 H(z)
                              │
                              ↓
估计的回声 = Ĥ(z) * 远端参考信号
清除后信号 = 混合信号 - 估计的回声
```

### NLMS 算法步骤

1. **滤波**：使用当前滤波器系数计算估计的回声
2. **误差计算**：期望信号 - 估计回声
3. **系数更新**：根据误差和步长因子更新滤波器系数

---

## 相关源文件

| 文件路径 | 作用 |
|----------|------|
| `src/switch_ivr_async.c` | 回声消除主要实现，preprocess 回调函数 |
| `src/include/switch_ivr.h` | `switch_ivr_preprocess_session()` 接口声明 |
| `libs/speex/libspeex/mdf.c` | Speex MDF（多延迟块频域）AEC 算法实现 |
| `libs/speex/include/speex/speex_echo.h` | Speex 回声消除 API 头文件 |
| `libs/speex/include/speex/speex_preprocess.h` | Speex 预处理 API 头文件 |

---

## 调试与日志

启用详细日志查看回声消除状态：

```xml
<!-- 在 freeswitch.xml 或日志配置中 -->
<param name="loglevel" value="debug"/>
```

日志输出示例：

```
[DEBUG] Setting AGC on r to 1
[DEBUG] Setting NOISE_SUPPRESS on r to -30 [0]
[DEBUG] Setting ECHO_SUPPRESS on r to -40 [0]
```

---

## 性能考虑

1. **CPU 占用**：回声消除是计算密集型操作，长尾长会增加 CPU 负载
2. **延迟**：处理本身会增加少量延迟（通常 < 1ms）
3. **内存**：每个会话约需几 KB 的缓冲区
4. **并发**：大量并发会话时需注意系统资源

### 性能优化建议

- 根据实际回声延迟选择合适的尾长
- 不需要时及时停止预处理
- 对于已知无回声的场景（如纯 IP 通话），可考虑禁用

---

## 参考资料

- [Speex DSP 手册](https://www.speex.org/docs/manual/speex-manual/node7.html)
- [FreeSWITCH Wiki - Preprocess](https://freeswitch.org/confluence/display/FREESWITCH/preprocess)
- [声学回声消除原理](https://en.wikipedia.org/wiki/Echo_cancellation)
