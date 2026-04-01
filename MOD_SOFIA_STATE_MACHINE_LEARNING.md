# mod_sofia / 状态机学习手册（基于当前源码）

本文目标：把 `mod_sofia`（SIP 端点）和 FreeSWITCH 核心状态机连接起来，形成一条可用于排障和开发的主链路。

## 1. 模块入口与挂接关系

主文件：

- [src/mod/endpoints/mod_sofia/mod_sofia.c](src/mod/endpoints/mod_sofia/mod_sofia.c)
- [src/mod/endpoints/mod_sofia/sofia.c](src/mod/endpoints/mod_sofia/sofia.c)
- [src/mod/endpoints/mod_sofia/sofia_glue.c](src/mod/endpoints/mod_sofia/sofia_glue.c)

### 1.1 模块生命周期入口

- `SWITCH_MODULE_LOAD_FUNCTION(mod_sofia_load)`  [src/mod/endpoints/mod_sofia/mod_sofia.c#L7008](src/mod/endpoints/mod_sofia/mod_sofia.c#L7008)
- `SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_sofia_shutdown)`  [src/mod/endpoints/mod_sofia/mod_sofia.c#L7458](src/mod/endpoints/mod_sofia/mod_sofia.c#L7458)

### 1.2 endpoint 接口挂接

在 load 中将 sofia endpoint 接入内核：

- `sofia_endpoint_interface->io_routines = &sofia_io_routines`  [src/mod/endpoints/mod_sofia/mod_sofia.c#L7289](src/mod/endpoints/mod_sofia/mod_sofia.c#L7289)
- `sofia_endpoint_interface->state_handler = &sofia_event_handlers`  [src/mod/endpoints/mod_sofia/mod_sofia.c#L7290](src/mod/endpoints/mod_sofia/mod_sofia.c#L7290)

这两行是理解整个模块的关键：

1. IO 路径走 `sofia_io_routines`
2. 状态机回调走 `sofia_event_handlers`

## 2. `sofia_io_routines` 与 `sofia_event_handlers`

定义位置：

- `switch_io_routines_t sofia_io_routines`  [src/mod/endpoints/mod_sofia/mod_sofia.c#L4979](src/mod/endpoints/mod_sofia/mod_sofia.c#L4979)
- `switch_state_handler_table_t sofia_event_handlers`  [src/mod/endpoints/mod_sofia/mod_sofia.c#L4995](src/mod/endpoints/mod_sofia/mod_sofia.c#L4995)

### 2.1 IO routines（媒体/信令 I/O）

重点入口：

- `sofia_outgoing_channel`（外呼建链）[src/mod/endpoints/mod_sofia/mod_sofia.c#L5069](src/mod/endpoints/mod_sofia/mod_sofia.c#L5069)
- `sofia_read_frame` / `sofia_write_frame`
- `sofia_receive_message` / `sofia_receive_event`

### 2.2 State handlers（状态机回调）

重点回调：

- `sofia_on_init` [src/mod/endpoints/mod_sofia/mod_sofia.c#L92](src/mod/endpoints/mod_sofia/mod_sofia.c#L92)
- `sofia_on_routing` [src/mod/endpoints/mod_sofia/mod_sofia.c#L152](src/mod/endpoints/mod_sofia/mod_sofia.c#L152)
- `sofia_on_execute` [src/mod/endpoints/mod_sofia/mod_sofia.c#L209](src/mod/endpoints/mod_sofia/mod_sofia.c#L209)
- `sofia_on_hangup` [src/mod/endpoints/mod_sofia/mod_sofia.c#L429](src/mod/endpoints/mod_sofia/mod_sofia.c#L429)
- `sofia_on_destroy` [src/mod/endpoints/mod_sofia/mod_sofia.c#L393](src/mod/endpoints/mod_sofia/mod_sofia.c#L393)

含义：这些函数由 core 状态机在特定 `CS_*` 状态触发，不是 SIP 栈直接调用。

## 3. Core 状态机如何驱动 sofia

核心循环入口：

- `switch_core_session_run(...)`  [src/switch_core_state_machine.c#L613](src/switch_core_state_machine.c#L613)

核心机制：

1. 读取 `session->endpoint_interface->state_handler`
2. 按当前 `switch_channel_state_t` 调 driver/app/default handler
3. 状态切换由 `switch_channel_set_state(...)` 发起

状态转移合法性检查：

- `switch_channel_perform_set_state(...)`  [src/switch_channel.c#L2420](src/switch_channel.c#L2420)

这段代码定义了从 `CS_NEW/CS_INIT/CS_ROUTING/...` 到下一个状态的合法边。

## 4. SIP 事件到状态动作的关键路径

SIP 事件分发主入口在 sofia 侧 switch-case：

- `nua_r_invite` -> `sofia_handle_sip_r_invite(...)`  [src/mod/endpoints/mod_sofia/sofia.c#L1854](src/mod/endpoints/mod_sofia/sofia.c#L1854)
- `nua_i_invite` -> `sofia_handle_sip_i_invite(...)`  [src/mod/endpoints/mod_sofia/sofia.c#L1888](src/mod/endpoints/mod_sofia/sofia.c#L1888)
- `nua_i_bye` -> `sofia_handle_sip_i_bye(...)`  [src/mod/endpoints/mod_sofia/sofia.c#L1862](src/mod/endpoints/mod_sofia/sofia.c#L1862)
- `nua_i_state` -> `sofia_handle_sip_i_state(...)`  [src/mod/endpoints/mod_sofia/sofia.c#L1909](src/mod/endpoints/mod_sofia/sofia.c#L1909)

对应实现：

- `sofia_handle_sip_r_invite` 定义 [src/mod/endpoints/mod_sofia/sofia.c#L7052](src/mod/endpoints/mod_sofia/sofia.c#L7052)
- `sofia_handle_sip_i_state` 定义 [src/mod/endpoints/mod_sofia/sofia.c#L7919](src/mod/endpoints/mod_sofia/sofia.c#L7919)
- `sofia_handle_sip_i_invite` 定义 [src/mod/endpoints/mod_sofia/sofia.c#L11353](src/mod/endpoints/mod_sofia/sofia.c#L11353)

理解方式：

- NUA 回调负责“信令解释与会话变量更新”
- Channel 状态机负责“执行时序和资源生命周期”
- 两者通过 `switch_channel_set_state` / message/event 连接

## 5. 外呼主链路（建议先读）

1. `sofia_outgoing_channel(...)` 创建 outbound session/channel
2. `sofia_glue_do_invite(...)` 组装并发送 INVITE
   - [src/mod/endpoints/mod_sofia/sofia_glue.c#L1034](src/mod/endpoints/mod_sofia/sofia_glue.c#L1034)
3. 收到 `nua_r_invite` 后进入 `sofia_handle_sip_r_invite(...)`
4. 根据 1xx/2xx/4xx 等响应更新通道变量与后续动作
5. core 状态机继续推进 `CS_INIT -> CS_ROUTING/CS_EXECUTE -> ...`

## 6. 入呼主链路（建议第二阶段读）

1. SIP 栈触发 `nua_i_invite`
2. `sofia_handle_sip_i_invite(...)` 解析 INVITE、创建/绑定会话私有对象
3. 根据 profile/channel 条件进入路由与执行阶段
4. 异常或结束时由 `nua_i_bye` / hangup 路径收敛

## 7. 状态机阅读重点（配合 mod_sofia）

优先理解这些状态：

- `CS_INIT`: 初始化
- `CS_ROUTING`: 查路由
- `CS_EXECUTE`: 执行 dialplan/app
- `CS_HANGUP`: 进入挂断
- `CS_REPORTING`: CDR 收敛
- `CS_DESTROY`: 生命周期结束

状态定义和注释在：

- [src/include/switch_types.h](src/include/switch_types.h)
- `CS_*` 说明约在 1423 附近

## 8. 与 event 模块的连接点

`mod_sofia_load()` 中做了大量事件子类注册与绑定，例如：

- `switch_event_reserve_subclass(MY_EVENT_...)`
- `switch_event_bind(modname, SWITCH_EVENT_CUSTOM, ...)`
- `switch_event_bind(modname, SWITCH_EVENT_PRESENCE_*, ...)`

位置：

- [src/mod/endpoints/mod_sofia/mod_sofia.c#L7008](src/mod/endpoints/mod_sofia/mod_sofia.c#L7008)

这也是你后续做 SIP 功能扩展时最常改的切入点。

## 9. 常见排障路径（按问题类型）

### 9.1 INVITE 发出但无响应

1. 看 `sofia_glue_do_invite` 是否执行
2. 看 `nua_r_invite` 是否进入
3. 看 channel 变量 `sip_*` 是否被设置

### 9.2 通话异常挂断

1. 看 `nua_i_bye/nua_r_bye` 分支
2. 看 `sofia_on_hangup`
3. 看 core 状态是否按 `CS_HANGUP -> CS_REPORTING -> CS_DESTROY` 推进

### 9.3 状态卡死

1. 看 `switch_channel_perform_set_state` 是否打印 invalid transition
2. 看 `switch_core_session_run` 中 `running_state` 是否更新
3. 看 endpoint state_handler 是否返回阻塞/异常

## 10. 建议学习节奏（可直接执行）

### Day 1

1. 通读 [src/mod/endpoints/mod_sofia/mod_sofia.c](src/mod/endpoints/mod_sofia/mod_sofia.c) 的 1-250 与 4979+ 区域
2. 抄出 `io_routines` 与 `state_handlers` 表，画一张调用图

### Day 2

1. 通读 [src/mod/endpoints/mod_sofia/sofia.c](src/mod/endpoints/mod_sofia/sofia.c) 的 NUA 分发 switch-case（1850 左右）
2. 深读 `sofia_handle_sip_r_invite` 与 `sofia_handle_sip_i_invite`

### Day 3

1. 通读 [src/switch_core_state_machine.c](src/switch_core_state_machine.c) `switch_core_session_run`
2. 通读 [src/switch_channel.c](src/switch_channel.c) `switch_channel_perform_set_state`
3. 对照一次真实呼叫日志把状态走一遍

## 11. 后续改代码的建议顺序

1. 先改 `sofia.c` 的分支动作（最小改动）
2. 再改 `sofia_glue.c` 的 SIP 报文拼装/变量映射
3. 最后才碰 `mod_sofia.c` 的全局流程和线程/队列行为

因为越往后影响面越大，回归成本越高。
