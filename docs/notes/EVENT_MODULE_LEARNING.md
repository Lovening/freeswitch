# FreeSWITCH Event 模块学习手册

本文基于当前仓库源码整理，目标是让你后续能按“定位 -> 理解 -> 改动 -> 验证”的方式快速进入 event 子系统。

## 1. 模块定位与职责

- 核心文件: [src/switch_event.c](src/switch_event.c)
- 头文件接口: [src/include/switch_event.h](src/include/switch_event.h)
- 会话侧私有事件队列入口: [src/switch_core_session.c](src/switch_core_session.c)

`switch_event` 负责三件事：

1. 事件对象创建/销毁（`switch_event_t` 生命周期）
2. 订阅关系管理（`EVENT_NODES[]` 链表）
3. 事件异步派发（全局队列 + 派发线程）

## 2. 全局数据结构（先看这里）

在 [src/switch_event.c](src/switch_event.c) 约 85-110 行：

- `EVENT_NODES[SWITCH_EVENT_ALL + 1]`: 按 `event_id` 分桶的订阅链表
- `RWLOCK`: 订阅链表读写锁（派发读锁，bind/unbind 写锁）
- `EVENT_DISPATCH_QUEUE`: 事件派发队列
- `EVENT_DISPATCH_QUEUE_THREADS[MAX_DISPATCH_VAL]`: 派发线程数组（默认上限 64）
- `DISPATCH_THREAD_COUNT` / `SOFT_MAX_DISPATCH`: 动态扩容派发线程计数

这几个变量就是系统行为的“总控面板”。

## 3. 事件生命周期（最重要）

### 3.1 创建

- `switch_event_create_subclass_detailed(...)`  [src/switch_event.c#L818](src/switch_event.c#L818)
- 常用宏 `switch_event_create(...)` 会展开到上面的 detailed 版本

动作：

1. 分配 `switch_event_t`
2. 初始化 `event_id`
3. 预填 file/function/line 调试信息
4. 可选写入 `Event-Subclass`

### 3.2 触发

- `switch_event_fire_detailed(...)`  [src/switch_event.c#L2305](src/switch_event.c#L2305)

动作：

1. 检查系统是否 running
2. 若 `runtime.events_use_dispatch` 为真，走异步队列
3. 调 `switch_event_queue_dispatch_event(...)` 入队

注意：`switch_event_fire(&event)` 之后，`event` 所有权转移，通常不再由调用方使用。

### 3.3 入队与线程派发

- `switch_event_queue_dispatch_event(...)`  [src/switch_event.c#L376](src/switch_event.c#L376)
- `switch_event_dispatch_thread(...)`  [src/switch_event.c#L313](src/switch_event.c#L313)

动作：

1. 根据队列压力决定是否扩容派发线程
2. `switch_queue_push(EVENT_DISPATCH_QUEUE, event)` 入队
3. 线程从队列 pop 后调用 `switch_event_deliver(...)`

### 3.4 投递与销毁

- `switch_event_deliver(...)`  [src/switch_event.c#L424](src/switch_event.c#L424)

动作：

1. 读锁遍历订阅链表
2. 先派发精确 `event_id`，再派发 `SWITCH_EVENT_ALL`
3. 调用命中的 `node->callback(event)`
4. 最后 `switch_event_destroy(event)`

## 4. 订阅与解绑（改功能常用）

### 4.1 绑定

- `switch_event_bind_removable(...)`  [src/switch_event.c#L2366](src/switch_event.c#L2366)
- `switch_event_bind(...)`  [src/switch_event.c#L2443](src/switch_event.c#L2443)

绑定本质：把 `switch_event_node_t` 头插到 `EVENT_NODES[event]`。

### 4.2 解绑

- `switch_event_unbind_callback(...)`  [src/switch_event.c#L2450](src/switch_event.c#L2450)
- `switch_event_unbind(...)`  [src/switch_event.c#L2498](src/switch_event.c#L2498)

解绑本质：写锁下从链表摘节点并释放。

## 5. 匹配规则（为什么 callback 有时收不到）

匹配函数：`switch_events_match(...)` [src/switch_event.c#L234](src/switch_event.c#L234)

- `SWITCH_EVENT_ALL`: 匹配所有事件
- 指定 `event_id`: 只匹配该类型
- 指定 subclass: 需与 `event->subclass_name`（或 file:/func: 规则）匹配

排查 callback 不触发时，优先核对：`event_id + subclass + bind 时机`。

## 6. 并发与锁语义（改代码前必读）

- 派发路径：`RWLOCK` 读锁
- bind/unbind 路径：`RWLOCK` 写锁 + `BLOCK` 互斥
- 队列与线程计数：`EVENT_QUEUE_MUTEX`

风险点：

1. callback 做阻塞 IO，导致派发线程堆积
2. 队列深度持续升高，触发丢事件风险
3. callback 里做重绑定/复杂锁操作，可能放大锁竞争

## 7. 与其他模块的真实连接点

- `mod_event_socket`：把事件推到 ESL 客户端
  - [src/mod/event_handlers/mod_event_socket/mod_event_socket.c](src/mod/event_handlers/mod_event_socket/mod_event_socket.c)
- `mod_json_cdr`：消费 hangup/trap 事件生成 CDR
  - [src/mod/event_handlers/mod_json_cdr/mod_json_cdr.c](src/mod/event_handlers/mod_json_cdr/mod_json_cdr.c)
- `mod_sofia`：在 load 时注册大量自定义 subclass 与事件绑定
  - [src/mod/endpoints/mod_sofia/mod_sofia.c#L7008](src/mod/endpoints/mod_sofia/mod_sofia.c#L7008)

## 8. 建议学习顺序（按半天节奏）

1. 读 [src/include/switch_event.h](src/include/switch_event.h)：类型和 API
2. 读 [src/switch_event.c#L818](src/switch_event.c#L818)：创建
3. 读 [src/switch_event.c#L2305](src/switch_event.c#L2305)：fire
4. 读 [src/switch_event.c#L376](src/switch_event.c#L376) + [src/switch_event.c#L313](src/switch_event.c#L313)：队列与派发线程
5. 读 [src/switch_event.c#L424](src/switch_event.c#L424)：deliver
6. 读 [src/switch_event.c#L2366](src/switch_event.c#L2366)：bind/unbind

## 9. 调试清单（改 bug 时直接套）

1. 在 create/fire/deliver 三点加 DEBUG 日志，带 `event_id/subclass/uuid`
2. 观察队列深度与派发线程数
3. 检查 callback 执行时长（是否有阻塞）
4. 验证解绑是否在模块 shutdown 时完成

## 10. 你的当前改动案例（与事件学习配套）

你已在 [src/switch_speex.c#L443](src/switch_speex.c#L443) 修复 `switch_speex_destroy()` 里的 `SpeexPreprocessState` 泄漏。

建议把这次改动当作 event 模块后续改造模板：

- 先定位生命周期（create/init -> destroy）
- 再检查并发语义（谁持有对象、何时释放）
- 最后做最小改动 + 可重复验证
