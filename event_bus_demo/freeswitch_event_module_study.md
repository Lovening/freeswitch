# FreeSWITCH Event 模块源码学习笔记

本文档用于对照阅读 FreeSWITCH event 模块源码。它不复制大段源码，而是用“文件 + 行号范围 + 解释”的方式做逐行级别导读，方便你在 VSCode 里跳转源码后复习。

分析范围：

- `/home/user/workspace/freeswitch/src/include/switch_event.h`
- `/home/user/workspace/freeswitch/src/switch_event.c`
- `/home/user/workspace/freeswitch/src/include/switch_types.h`
- `/home/user/workspace/freeswitch/src/mod/event_handlers/mod_event_test/mod_event_test.c`
- `/home/user/workspace/freeswitch/tests/unit/switch_event.c`

说明：

- `switch_event.c` 共 4362 行，本文用连续行号区间覆盖全部文件。
- 对主链函数采用接近逐行解释：创建、补系统头、fire、入队、worker 出队、匹配 listener、回调、销毁。
- 对 JSON event channel/live array 这类扩展功能，用较细的连续行区间解释，先把主事件总线吃透。

## 一句话总览

FreeSWITCH event 模块是一个全局异步事件总线：

1. 生产者创建 `switch_event_t`。
2. 填 header/body。
3. 调用 `switch_event_fire(&event)`。
4. event 模块把事件交给 dispatch queue 或线程池。
5. 后台线程调用 `switch_event_deliver(&event)`。
6. `switch_event_deliver()` 扫描绑定表，匹配 listener，执行 callback。
7. delivery 结束后销毁 event。

核心数据流：

```text
switch_event_create()
  -> switch_event_prep_for_delivery()
  -> switch_event_add_header_string()
  -> switch_event_fire()
  -> switch_event_queue_dispatch_event()
  -> switch_event_dispatch_thread()
  -> switch_event_deliver()
  -> switch_events_match()
  -> node->callback(event)
  -> switch_event_destroy()
```

## 先看哪些文件

| 文件 | 作用 | 建议阅读顺序 |
|---|---|---|
| `src/include/switch_types.h` | 定义 `switch_event_types_t`、`switch_priority_t` | 1 |
| `src/include/switch_event.h` | 对外 API、核心结构体声明 | 2 |
| `src/switch_event.c` | event 模块实现 | 3 |
| `src/mod/event_handlers/mod_event_test/mod_event_test.c` | 最小 event handler 模块例子 | 4 |
| `tests/unit/switch_event.c` | header 添加/查询的单元测试 | 5 |

## 数据结构总图

```text
switch_event_t
  event_id          -> 事件类型，例如 SWITCH_EVENT_CHANNEL_EXECUTE
  priority          -> 优先级字段，注意这份源码不按 priority 重排 dispatch queue
  subclass_name     -> CUSTOM/CLONE 等场景的子类名
  headers           -> switch_event_header_t 链表头
  last_header       -> header 链表尾，便于 O(1) 追加
  body              -> 可选正文
  bind_user_data    -> listener 绑定时传入的数据，deliver 时写入
  event_user_data   -> fire 时传入的数据
  flags             -> EF_UNIQ_HEADERS 等行为标志

switch_event_header_t
  name/value        -> header 名和值
  array/idx         -> 支持同名多值 header
  hash              -> header name 的大小写无关 hash
  next              -> 单链表

switch_event_node_t
  id                -> 绑定者标识，通常是模块名
  event_id          -> 订阅哪类事件
  subclass_name     -> 订阅哪个 subclass，可为 NULL
  callback          -> 事件回调
  user_data         -> 回调上下文
  next              -> 同类型事件的 listener 链表
```

全局索引：

```text
EVENT_NODES[SWITCH_EVENT_ALL + 1]
  每个事件类型一个 listener 链表
  deliver 时先扫 event_id 对应链表，再扫 SWITCH_EVENT_ALL 链表

CUSTOM_HASH
  custom subclass 注册表，避免多个模块抢同一个 subclass

EVENT_DISPATCH_QUEUE
  异步投递队列，worker 从里面 pop 出 switch_event_t
```

## 重要结论

### 1. priority 不是 dispatch 调度优先级

`switch_event_set_priority()` 位于 `switch_event.c:867-873`：

- `870`：写入 `event->priority`。
- `871`：增加 `"priority"` header。

但 `switch_event_queue_dispatch_event()` 在 `switch_event.c:376-420` 使用的是普通 `switch_queue_push(EVENT_DISPATCH_QUEUE, event)`，没有按 `event->priority` 拆队列或排序。

也就是说，在这份源码里 priority 更像事件元数据，而不是调度算法。我们 demo 后来加的 `HIGH/NORMAL/LOW` 三队列，是一个增强版设计，不是 FreeSWITCH 当前代码的原样行为。

### 2. callback 必须很快返回

`switch_event.h:44-48` 已经说明：如果 callback 预计会耗时，应该自己再建线程和 FIFO，把事件快速搬走，不能阻塞核心 delivery agent。

原因在 `switch_event_deliver()`：

- `431`：拿 `RWLOCK` 读锁。
- `434-440`：遍历 listener 并同步调用 callback。
- callback 不返回，后面的 listener 和队列 worker 就会被拖住。

### 3. event 的所有权通过 `switch_event_t **` 转移

`switch_event_fire(&event)` 传的是二级指针。成功入队后，`switch_event_queue_dispatch_event()` 会把 `*eventp = NULL`，表示调用者不再拥有它。最终由 `switch_event_deliver()` 调 `switch_event_destroy()`。

这是 FreeSWITCH event 模块非常核心的 C 语言所有权技巧。

## `switch_types.h` 相关枚举

### `switch_priority_t`：`switch_types.h:1105-1117`

| 行号 | 作用 |
|---|---|
| 1105-1111 | Doxygen 注释，说明 NORMAL/LOW/HIGH 三种优先级。 |
| 1113-1117 | enum 定义：`SWITCH_PRIORITY_NORMAL`、`LOW`、`HIGH`。 |

### `switch_event_types_t`：`switch_types.h:2030-2225`

| 行号 | 作用 |
|---|---|
| 2030-2129 | 事件类型说明，包含 channel、system、presence、message、media 等事件。 |
| 2130 | enum 开始。 |
| 2131 | `SWITCH_EVENT_CUSTOM`，自定义事件入口。 |
| 2132 | `SWITCH_EVENT_CLONE`，用于复制/反序列化/内部构造事件。 |
| 2133-2153 | channel 生命周期事件，例如 create、destroy、answer、hangup、execute、bridge。 |
| 2155-2188 | 系统和模块事件，例如 API、LOG、STARTUP、SHUTDOWN、MODULE_LOAD、HEARTBEAT。 |
| 2189-2204 | message、request、RTCP、client/server disconnect 等事件。 |
| 2205-2223 | NAT、record/playback、conference、device、text、shutdown requested 等事件。 |
| 2224 | `SWITCH_EVENT_ALL`，不是具体事件，而是“监听所有事件”的哨兵值。 |

关键设计：`SWITCH_EVENT_ALL` 放在最后。`EVENT_NODES[SWITCH_EVENT_ALL + 1]` 就能覆盖所有事件类型。

## `switch_event.h` 行号导读

| 行号 | 内容 | 解释 |
|---|---|---|
| 1-31 | 版权和文件说明 | MPL 1.1 许可证，文件名说明为 Event System。 |
| 32-50 | Doxygen 总说明 | 明确 event system 使用后台线程和 APR threadsafe FIFO queue。 |
| 52-63 | include guard 和 C/C++ extern | `SWITCH_BEGIN_EXTERN_C` 让 C++ 调用 C ABI。 |
| 64-77 | `struct switch_event_header` | header 节点，包含 name/value/array/hash/next。 |
| 80-103 | `struct switch_event` | event 主体，包含 event_id、priority、subclass、headers、body、userdata、flags。 |
| 105-117 | binary serialize 结构 | libtpl 二进制序列化时使用的扁平结构。 |
| 119-123 | `switch_event_flag_t` | `EF_UNIQ_HEADERS` 等事件行为标志。 |
| 126-128 | 前置声明和 `SWITCH_EVENT_SUBCLASS_ANY` | `NULL` 表示不过滤 subclass。 |
| 130-154 | init、shutdown、create_subclass | event 系统生命周期和创建 API。 |
| 153 | `switch_event_create_subclass` 宏 | 自动带上 `__FILE__`、函数名、行号。 |
| 155-183 | priority、get header/body、rename | event 属性访问 API。 |
| 185-213 | add/del header、array header | header 写入 API，支持 varargs、string、nodup、array。 |
| 214-229 | destroy、dup、merge、reply | 内存释放、事件复制、回复事件构造。 |
| 231-243 | fire 和 prep | 发送事件和补系统 header 的核心 API。 |
| 246-278 | bind/unbind | listener 订阅和取消订阅。 |
| 280-305 | name/event 映射、custom subclass | 字符串名映射 enum，自定义事件 subclass 注册。 |
| 307-333 | serialize/json/xml/brackets | 多种传输格式转换。 |
| 335-355 | running/body/expand headers | 运行态检测和花括号变量展开。 |
| 357-398 | presence 便利函数、plain create | presence 事件构造，以及不自动 prep 的 `switch_event_create_plain()`。 |
| 400-429 | deliver、fire 宏、工具 API | `switch_event_fire()` 宏自动记录调用点。 |
| 431-462 | event_channel/live_array API | JSON channel 和 live array 状态同步 API。 |
| 464-478 | 文件尾部编辑器配置 | Emacs/Vim 缩进配置。 |

## `switch_event.c` 全文件覆盖索引

| 行号 | 主题 | 解释 |
|---|---|---|
| 1-43 | 版权、include、可选 tpl | 引入 `switch.h`、私有 core 头、`switch_event.h`，可选 libtpl。 |
| 44-46 | dispatch 队列配置 | `DISPATCH_QUEUE_LEN=10000`，可选 debug 宏。 |
| 47-61 | `switch_event_node` | listener 链表节点。 |
| 63-72 | `switch_event_subclass` | custom subclass 注册记录。 |
| 74-82 | `event_channel_manager` | JSON event channel 管理器。 |
| 84-111 | 全局状态 | worker 数、队列、锁、hash、sequence、运行态。 |
| 113-134 | 前置声明和内存宏 | `unsub_all...`、`my_dup()`、`ALLOC/DUP/FREE`。 |
| 136-232 | `EVENT_NAMES` | enum 到事件名字符串的数组，必须和 `switch_event_types_t` 同步。 |
| 234-285 | `switch_events_match()` | 判断 event 是否匹配 listener。 |
| 287-311 | 线程池单事件投递 | 不使用 dispatch queue 时，每个 event 扔线程池。 |
| 313-372 | `switch_event_dispatch_thread()` | dispatch worker 主循环。 |
| 374-420 | `switch_event_queue_dispatch_event()` | 入 dispatch queue，必要时扩 worker。 |
| 423-452 | `switch_event_deliver()` | 遍历 listener，执行 callback，最后销毁 event。 |
| 454-486 | running/name 映射 | 运行态检测、event enum 与字符串互转。 |
| 488-565 | custom subclass reserve/free | subclass 注册表。 |
| 567-592 | event recycle reclaim | 仅 `SWITCH_EVENT_RECYCLE` 打开时有效。 |
| 594-690 | `switch_event_shutdown()` | 停 worker、清队列、清 hash。 |
| 692-758 | dispatch queue 初始化和扩容 | `check_dispatch()` 与 `switch_event_launch_dispatch_threads()`。 |
| 760-815 | `switch_event_init()` | 初始化锁、hash、IP、队列、运行态。 |
| 817-865 | event 创建 | 分配 event，设置 flag，补系统 header。 |
| 867-873 | priority 设置 | 写字段并加 `"priority"` header。 |
| 875-1009 | header rename/get/del | header 链表查询、重命名、删除。 |
| 1011-1072 | header 分配与释放 | 支持 recycle，释放 array/value/name。 |
| 1074-1370 | 添加 header 核心逻辑 | 处理 `_body`、数组、重复 header、栈插入策略。 |
| 1372-1465 | add header/body 包装函数 | varargs、string、nodup、body API。 |
| 1467-1636 | destroy/merge/dup/reply | event 生命周期和复制逻辑。 |
| 1638-1740 | binary serialize | 可选 libtpl。 |
| 1742-2270 | text/json/xml/brackets serialize | 各类格式转换。 |
| 2272-2302 | prep 系统 header | 补 Event-Name、Core-UUID、时间、调用点、序列号。 |
| 2304-2341 | fire | 运行态检查，选择 dispatch queue 或线程池。 |
| 2343-2540 | get custom/bind/unbind | 订阅表管理。 |
| 2542-2570 | presence 事件便利创建 | 生成并发送 `SWITCH_EVENT_PRESENCE_IN`。 |
| 2572-3060 | header 展开和参数串 | 花括号变量、API 执行、URL 参数拼接。 |
| 3062-3162 | 权限和 presence data cols | expansion 权限、presence 额外字段。 |
| 3164-3698 | JSON event channel | channel 订阅、广播、异步队列、权限。 |
| 3700-4350 | live array | 基于 event channel 的 JSON 状态数组同步。 |
| 4352-4362 | 编辑器配置 | 缩进设置。 |

## 核心函数逐行级解析

### `switch_events_match()`：`switch_event.c:234-285`

| 行号 | 解释 |
|---|---|
| 234 | 定义静态匹配函数，只在本文件内部使用。 |
| 236 | `match` 初始为 0。 |
| 238-246 | 如果 listener 订阅 `SWITCH_EVENT_ALL`，先认为类型匹配。没有 subclass 过滤就直接返回匹配。 |
| 248-249 | 如果前面已经匹配，或者 event_id 精确相等，进入 subclass 判断。 |
| 251-273 | event 和 listener 都有 subclass 时，继续判断 subclass。 |
| 253-260 | listener subclass 支持特殊格式 `file:xxx`，实际拿 event header `"file"` 比较。 |
| 261-268 | listener subclass 支持特殊格式 `func:xxx`，实际拿 event header `"function"` 比较。 |
| 269-272 | 普通 subclass 直接字符串完全匹配。 |
| 274-277 | event 有 subclass 但 listener 没有，或者双方都没有 subclass，也算匹配。 |
| 278-281 | 其他情况不匹配。 |
| 284 | 返回匹配结果。 |

整体联系：`switch_event_deliver()` 每次遍历 listener 时都调用这个函数。它是“事件过滤器”的核心。

### `switch_event_dispatch_thread()`：`switch_event.c:313-372`

| 行号 | 解释 |
|---|---|
| 313-315 | worker 线程入口，参数是 `switch_queue_t *`。 |
| 316 | `my_id` 用于记录自己在全局线程数组中的下标。 |
| 318-320 | 加锁后增加全局线程计数和 dispatch 线程计数。 |
| 322-328 | 在 `EVENT_DISPATCH_QUEUE_THREADS` 里找当前线程的 index。 |
| 330-334 | 找不到 index，说明状态异常，解锁返回。 |
| 336-337 | 标记当前 worker 已运行，释放锁。 |
| 339-362 | worker 主循环。 |
| 344-347 | 系统不运行时退出。 |
| 349-352 | 从队列阻塞 pop；失败则继续等待。 |
| 354-357 | pop 到 NULL 表示关闭哨兵，退出。 |
| 359-360 | 将队列元素转为 `switch_event_t *`，同步 deliver。 |
| 361 | `switch_os_yield()` 主动让出 CPU。 |
| 364-368 | 退出前维护运行状态和线程计数。 |
| 370-371 | 打日志并返回。 |

整体联系：这是异步事件系统的消费者。所有 `switch_event_fire()` 入队的事件最终都在这里被取出。

### `switch_event_queue_dispatch_event()`：`switch_event.c:376-420`

| 行号 | 解释 |
|---|---|
| 376-380 | 参数是 `switch_event_t **`，准备接管 event 所有权。 |
| 381-384 | 系统没运行则返回失败。 |
| 386-418 | 理论上支持链式 event，但当前逻辑每次只处理一个。 |
| 390 | 给队列状态加锁。 |
| 392-399 | 如果队列长度超过 `DISPATCH_QUEUE_LEN * DISPATCH_THREAD_COUNT`，尝试增加 worker。 |
| 401 | 释放锁。 |
| 403-413 | 如果需要扩容，调用 `switch_event_launch_dispatch_threads()`，再减少 `PENDING`。 |
| 415 | `*eventp = NULL`，调用者不再拥有 event。 |
| 416 | `switch_queue_push()` 入 FIFO 队列。 |
| 417 | 本地指针清空，避免重复处理。 |
| 420 | 返回成功。 |

高级点：用二级指针表达所有权转移。这是 C 项目里非常重要的资源管理手法。

### `switch_event_deliver()`：`switch_event.c:423-452`

| 行号 | 解释 |
|---|---|
| 423-424 | 对外可见函数，可以直接同步投递 event。 |
| 426-427 | 准备遍历事件类型和 listener 节点。 |
| 429 | 只有系统运行时才派发 callback。 |
| 431 | 拿全局 `RWLOCK` 读锁，允许多个 deliver 并发读 listener 表。 |
| 432 | 先遍历 event 自己的 `event_id`，下一轮强制切到 `SWITCH_EVENT_ALL`。 |
| 434 | 遍历当前 event type 下的 listener 链表。 |
| 436 | 调用 `switch_events_match()` 做 subclass 过滤。 |
| 438 | 把 listener 的 `user_data` 写到 event 的 `bind_user_data`。 |
| 439 | 同步调用 listener callback。 |
| 443-446 | 扫完 `SWITCH_EVENT_ALL` 后跳出循环。 |
| 448 | 释放读锁。 |
| 451 | deliver 结束后销毁 event。 |

注意：回调是在读锁内执行的。这个设计让 listener 表不会在遍历时被 unbind 改掉，但也意味着 callback 必须短。

### `switch_event_init()`：`switch_event.c:760-815`

| 行号 | 解释 |
|---|---|
| 764-769 | `MAX_DISPATCH = CPU/2 + 1`，且至少 2。 |
| 771-772 | 要求外部传入 pool，并保存到运行期 pool。 |
| 773-777 | 创建全局 RWLOCK、多个 mutex。 |
| 778 | 初始化 `CUSTOM_HASH`。 |
| 780-783 | minimal 模式提前返回，不启完整 event 引擎。 |
| 785 | 日志：Activate Eventing Engine。 |
| 787-793 | 初始化 event channel 管理器。 |
| 795-797 | `SYSTEM_RUNNING = -1`，表示初始化中。 |
| 800-801 | 猜测本机 IPv4/IPv6，用于系统 event header。 |
| 803-806 | 可选 recycle queue。 |
| 808 | 初始化 dispatch queue 和第一个 worker。 |
| 810-812 | `SYSTEM_RUNNING = 1`，正式开始接收事件。 |
| 814 | 返回成功。 |

整体联系：`switch_core.c:2022` 调用它。FreeSWITCH core 初始化事件系统后，模块才能 bind/fire event。

### `check_dispatch()` 和 `switch_event_launch_dispatch_threads()`：`switch_event.c:692-758`

| 行号 | 解释 |
|---|---|
| 692-710 | 懒初始化 dispatch queue，双重检查 `EVENT_DISPATCH_QUEUE`。 |
| 700 | 创建容量为 `DISPATCH_QUEUE_LEN * MAX_DISPATCH` 的队列。 |
| 701 | 启动第一个 dispatch worker。 |
| 703-706 | 等待 worker 线程计数起来。 |
| 712-758 | 根据 `max` 启动更多 dispatch worker。 |
| 723-731 | 防止超过硬上限，防止缩容。 |
| 733-755 | 创建线程属性、设置栈大小、设置实时优先级、启动线程。 |
| 744-745 | 等待新线程标记 running。 |
| 757 | 更新 `SOFT_MAX_DISPATCH`。 |

设计点：队列懒创建，worker 可扩展，但不轻易缩容。

### `switch_event_create_subclass_detailed()`：`switch_event.c:817-865`

| 行号 | 解释 |
|---|---|
| 825 | 先把输出指针置 NULL。 |
| 827-830 | 只有 `CLONE` 和 `CUSTOM` 允许传 subclass，否则返回错误。 |
| 831-843 | 如果开启 recycle，优先复用旧 event；否则 malloc。 |
| 845 | 清零 event。 |
| 847-850 | REQUEST_PARAMS、CHANNEL_DATA、MESSAGE 默认开启唯一 header。 |
| 852-856 | 非 CLONE 事件设置 event_id，并立即 prep 系统 header。 |
| 858-862 | 有 subclass 时保存 `subclass_name`，并加 `Event-Subclass` header。 |
| 864 | 返回成功。 |

整体联系：普通宏 `switch_event_create(event, id)` 最终走这里，并自动记录调用文件、函数、行号。

### `switch_event_prep_for_delivery_detailed()`：`switch_event.c:2272-2302`

| 行号 | 解释 |
|---|---|
| 2275-2279 | 准备时间结构、日期字符串、当前微秒时间、序列号。 |
| 2281-2283 | 加锁递增全局 `EVENT_SEQUENCE_NR`。 |
| 2285 | 加 `Event-Name`。 |
| 2286 | 加 `Core-UUID`。 |
| 2287-2288 | 加 FreeSWITCH 主机名和 switchname。 |
| 2289-2290 | 加猜测出的 IPv4/IPv6。 |
| 2292-2296 | 生成本地时间和 GMT 时间 header。 |
| 2297 | 加微秒时间戳。 |
| 2298-2300 | 加调用点文件、函数、行号。 |
| 2301 | 加事件序列号。 |

整体联系：这就是为什么 ESL/event_socket 里看到的事件天然带 `Event-Date-*`、`Event-Sequence`、`Event-Calling-*`。

### `switch_event_fire_detailed()`：`switch_event.c:2304-2341`

| 行号 | 解释 |
|---|---|
| 2308-2311 | 断言 event 系统关键锁和 pool 已初始化。 |
| 2313-2318 | 系统未运行时销毁 event 并返回成功，避免调用方泄漏。 |
| 2320-2323 | 如果传了 `user_data`，写入 event。 |
| 2325-2334 | 使用 dispatch queue 模式：确保队列存在，然后入队。 |
| 2335-2338 | 非 dispatch 模式：扔到线程池单独投递。 |
| 2340 | 返回成功。 |

这段很短，说明它只负责“发送入口”，不负责匹配和回调。

### `switch_event_base_add_header()`：`switch_event.c:1121-1370`

这是 header 系统的核心函数。

| 行号 | 解释 |
|---|---|
| 1121-1128 | 函数入口，准备 header 指针、hash 长度、是否已存在、数组 index 等局部变量。 |
| 1130-1133 | 特殊 header `_body` 会转成 event body。 |
| 1135-1145 | 支持 `Header[3]` 这种数组索引语法，把真实 header name 切出来。 |
| 1147-1165 | 如果是数组索引、PUSH、UNSHIFT，优先找已有 header。 |
| 1151-1162 | 如果指定 index 但 header 不存在，就创建临时 header。 |
| 1156-1159 | 如果 event 开了 `EF_UNIQ_HEADERS`，先删同名 header。 |
| 1167-1206 | 按指定 index 写 array；必要时 realloc，填补空字符串，重绘 value。 |
| 1208-1218 | PUSH/UNSHIFT 下存在同名 header 时，转为 array 操作。 |
| 1222-1245 | 没找到 header 时，新建 header；空值会删除同名 header。 |
| 1232-1235 | 唯一 header 模式下，新增前删除旧值。 |
| 1237-1242 | 识别 `ARRAY::` 前缀，把序列化数组还原为多值 header。 |
| 1247-1284 | PUSH/UNSHIFT 增加 array 元素，必要时把原单值转换为 array。 |
| 1285-1329 | 根据 array 重新生成 `header->value`，格式是 `ARRAY::a|:b|:c`。 |
| 1331-1335 | 非数组模式下，直接替换 `header->value`。 |
| 1337-1363 | 新 header 插入链表：TOP 插头部，否则插尾部。 |
| 1365-1369 | 清理临时 header name，返回成功。 |

高级点：

- `last_header` 让尾插为 O(1)。
- `hash` 减少不必要的字符串比较。
- `PUSH/UNSHIFT/TOP/BOTTOM` 把 header 当成可控栈/队列使用。
- `nodup` 版本把 `data` 指针所有权交给 event，使用时要非常小心。

### `switch_event_destroy()`：`switch_event.c:1467-1493`

| 行号 | 解释 |
|---|---|
| 1470-1471 | 取出 event 和 header 遍历指针。 |
| 1473 | event 非 NULL 才释放。 |
| 1475-1480 | 遍历 header 链表，逐个 `free_header()`。 |
| 1481-1482 | 释放 body 和 subclass。 |
| 1483-1490 | 如果开启 recycle，把 event 放回 recycle queue；否则 free。 |
| 1492 | 把调用方指针置 NULL。 |

所有权设计再次出现：销毁函数也接收二级指针，防止悬挂指针。

### `switch_event_serialize()`：`switch_event.c:1742-1879`

| 行号 | 解释 |
|---|---|
| 1745-1750 | 准备输出 buffer、编码 buffer 和容量。 |
| 1751 | 输出字符串先置 NULL。 |
| 1753-1764 | 预分配正文 buffer 和 encode buffer。 |
| 1767-1832 | 遍历 header 并写成 `Name: Value\n` 格式。 |
| 1777-1789 | 如果 header 是 array，按每个元素估算编码空间。 |
| 1791-1804 | 编码 buffer 不够则扩容。 |
| 1808-1815 | encode=true 时 URL encode；否则用 bracket 风格包裹。 |
| 1817-1828 | 输出 buffer 不够则扩容。 |
| 1830-1831 | 写入一行 header。 |
| 1835 | 释放 encode buffer。 |
| 1837-1874 | 写 body；有 body 时补 `Content-Length`，否则写空行结束。 |
| 1876-1878 | 输出指针接管 buffer，返回成功。 |

整体联系：event_socket、日志、测试模块都会依赖这种文本格式。

### `switch_event_bind_removable()`：`switch_event.c:2365-2440`

| 行号 | 解释 |
|---|---|
| 2365-2370 | 函数入口，准备 listener 节点和 subclass 指针。 |
| 2372-2373 | 断言 event 系统已初始化。 |
| 2375-2378 | 如果调用方想拿 handle，先置 NULL。 |
| 2380-2403 | 有 subclass 时，确保 subclass 在 `CUSTOM_HASH` 中存在。 |
| 2386-2391 | listener 先 reserve subclass 时，会把 `bind=1`，表示这是监听者预占。 |
| 2397-2402 | subclass 无法 reserve 则失败。 |
| 2405 | event enum 必须在范围内。 |
| 2407 | 分配 `event_node`。 |
| 2408-2409 | 拿写锁和互斥锁，准备改全局 listener 表。 |
| 2411-2419 | 填 node 的 id、event_id、subclass、callback、user_data。 |
| 2421-2426 | 头插到 `EVENT_NODES[event]` 链表。 |
| 2427-2428 | 解锁。 |
| 2431-2434 | 如果调用方要 handle，返回 node。 |
| 2436 | 成功。 |

高级点：listener 表是“按 event_id 分桶的链表”，不是全局链表，所以 deliver 时只扫相关事件类型和 `ALL`。

### `switch_event_unbind()`：`switch_event.c:2497-2540`

| 行号 | 解释 |
|---|---|
| 2503-2508 | 取出目标 node；为空则失败。 |
| 2510-2512 | 写锁加互斥锁，准备修改链表。 |
| 2513-2534 | 遍历对应 event_id 的链表，找到目标 node 后摘链。 |
| 2525-2528 | 打日志并释放 subclass/id/node。 |
| 2529 | 调用方 handle 置 NULL。 |
| 2535-2536 | 解锁。 |
| 2539 | 返回状态。 |

与 deliver 的关系：deliver 拿读锁，unbind 拿写锁。这样遍历回调时不会被链表修改打断。

## Custom subclass 机制

相关代码：

- `switch_event_reserve_subclass_detailed()`：`switch_event.c:524-565`
- `switch_event_free_subclass_detailed()`：`switch_event.c:488-522`
- `switch_event_bind_removable()`：`switch_event.c:2380-2403`

核心规则：

1. custom event 通常用 `module::event_name` 命名。
2. producer 模块可以 reserve subclass，防止别人抢名。
3. listener 如果先 bind 一个 subclass，代码也会帮它 reserve，但标记 `bind=1`。
4. 真正 owner 后续 reserve 时，如果看到 `bind=1`，可以接管。

这解释了 `mod_event_test.c:121`：

```text
switch_event_reserve_subclass(MY_EVENT_COOL)
```

其中 `MY_EVENT_COOL` 是 `"test::cool"`。

## JSON event_channel 和 live_array

这部分位于 `switch_event.c:3164-4350`，它不是传统 `switch_event_t` 主链，而是 JSON 发布/订阅和状态数组同步。

### event_channel

| 行号 | 主题 | 解释 |
|---|---|---|
| 3164-3180 | 订阅节点结构 | 一个 channel 下挂多个 callback。 |
| 3181-3221 | 按 head 取消订阅 | 可按 callback/user_data 删除节点。 |
| 3223-3253 | 全部取消订阅 | shutdown 时清空 channel 和 permission hash。 |
| 3255-3289 | 按 channel 取消订阅 | 支持传 NULL 表示所有 channel。 |
| 3291-3355 | 订阅 channel | hash 中没有 channel 就创建 head，再追加 node。 |
| 3357-3363 | `event_channel_data_t` | 异步广播时队列里传递的数据。 |
| 3365-3389 | `_switch_event_channel_broadcast()` | 在读锁内遍历订阅者并调用 JSON callback。 |
| 3391-3405 | `destroy_ecd()` | 释放 channel data。 |
| 3411-3485 | `ecd_deliver()` | 负责实际广播，支持层级 channel 和全局 channel。 |
| 3487-3530 | channel worker | 类似 event dispatch thread，但处理 JSON channel data。 |
| 3532-3596 | 异步 broadcast | 必要时启动 channel worker，然后 trypush 队列。 |
| 3598-3614 | 同步 deliver | 不入队，直接调用 `ecd_deliver()`。 |
| 3616-3639 | bind/unbind API | 对外封装订阅和取消订阅。 |
| 3641-3698 | permission | 基于 cookie 和 event_channel 的权限表。 |

### live_array

| 行号 | 主题 | 解释 |
|---|---|---|
| 3700-3735 | alias、node、live_array 结构 | live array 是带 hash、链表、序列号和 alias 的状态容器。 |
| 3737-3758 | `la_broadcast()` | 向主 channel 和 alias channel 广播 JSON。 |
| 3760-3784 | visible | 发 show/hide 类 JSON 消息。 |
| 3786-3820 | clear | 广播 clear，然后释放所有节点。 |
| 3822-3896 | bootstrap | 给新订阅者发送当前完整状态。 |
| 3898-3939 | destroy | 引用计数归零后清理 hash、alias、pool。 |
| 3941-4030 | alias 管理 | 添加/删除 alias，并维护 `lahash`。 |
| 4032-4077 | create | 通过 `event_channel.name` key 复用或创建 live array。 |
| 4079-4113 | get/get_idx | 返回 JSON deep copy。 |
| 4115-4125 | lock/unlock | 暴露内部 mutex 给调用方。 |
| 4127-4182 | del | 删除一个元素并广播 `del`。 |
| 4184-4298 | add | 添加或修改元素并广播 `add/modify`。 |
| 4300-4312 | user data / command handler | 设置回调上下文。 |
| 4314-4350 | parse_json | 解析远端 command，例如 bootstrap，交给 live array。 |

## 启动和关闭中的位置

### 启动：`switch_core.c:2022`

core 初始化阶段调用：

```text
switch_event_init(runtime.memory_pool)
```

这意味着 event 模块是基础设施，通道、模块、日志、ESL 等上层模块都可以依赖它。

### dispatch threads 配置：`switch_core.c:2387`

配置解析时可以调用：

```text
switch_event_launch_dispatch_threads(tmp)
```

用于手动提升 dispatch worker 数量。

### 关闭：`switch_core.c:3133`

core shutdown 时调用：

```text
switch_event_shutdown()
```

先让系统停止接收，再 interrupt queue、join worker、清理剩余 event 和 hash。

## 模块例子：`mod_event_test.c`

| 行号 | 解释 |
|---|---|
| 40-72 | `event_handler()`，收到 event 后序列化为 text 和 xml 并打印。 |
| 47-50 | 忽略 LOG 事件，避免日志事件递归刷屏。 |
| 51 | `switch_event_serialize()` 生成文本事件。 |
| 52-58 | `switch_event_xmlize()` 生成 XML 事件并输出。 |
| 74 | 定义 custom subclass：`test::cool`。 |
| 92-95 | torture 模式下创建 custom event、加 header、fire。 |
| 121-124 | 模块加载时 reserve subclass。 |
| 126-129 | bind `SWITCH_EVENT_ALL`，监听所有事件。 |

这个模块是学习 event handler 的最小入口。

## 单元测试：`tests/unit/switch_event.c`

| 行号 | 解释 |
|---|---|
| 20-104 | `benchmark` 测试。 |
| 43-44 | 创建 `SWITCH_EVENT_MESSAGE`。 |
| 47-50 | 循环添加 header。 |
| 69-71 | 循环读取 header 并校验。 |
| 88 | 销毁 event。 |
| 98-102 | 打印耗时和吞吐。 |

测试重点是 header add/get 的成本，不是 dispatch queue。

## 高级技巧清单

### 1. 宏记录调用点

`switch_event.h:153`、`413`、`422` 用宏把 `__FILE__`、函数名、`__LINE__` 注入详细函数。这样每个 event 都知道自己在哪里创建/发送。

### 2. 二级指针表达所有权

`switch_event_fire_detailed(..., switch_event_t **event, ...)`：

- 成功入队后置 `*event = NULL`。
- deliver 结束后 destroy。
- 调用者不需要也不应该再释放。

### 3. 读写锁保护 listener 表

- deliver 使用读锁。
- bind/unbind 使用写锁。

这样多个 worker 可以同时投递事件，但 listener 链表修改是独占的。

### 4. header 支持多值数组

同一个 header 可以通过 `SWITCH_STACK_PUSH` 形成 array，并序列化为 `ARRAY::a|:b` 风格。

### 5. header 查找预存 hash

`switch_event_header_t.hash` 存 name 的大小写无关 hash。查找时先比较 hash，再 `strcasecmp()`，减少字符串比较成本。

### 6. queue 扩 worker

`switch_event_queue_dispatch_event()` 根据队列积压动态增加 dispatch threads，但硬上限由 `MAX_DISPATCH` 控制。

### 7. recycle 条件编译

`SWITCH_EVENT_RECYCLE` 默认注释掉。如果打开，event/header 可以进 recycle queue，减少 malloc/free 压力。

### 8. custom subclass 注册表

`CUSTOM_HASH` 让 custom event subclass 有 ownership 概念，避免不同模块使用同名 custom event。

## 可疑点和阅读时要注意的地方

### `switch_event_create_pres_in_detailed()` 的 `unique-id`

`switch_event.c:2562` 写的是：

```text
"unique-id" <- alt_event_type
```

从参数名看，它可能本来想写 `unique_id`。这里只做阅读备注，不在源码里修改。

### `switch_event_set_priority()` 的 header 名

`switch_event.c:871` 添加的是 `"priority"`，不是 `"Event-Priority"`。这和我们 demo 里使用 `Event-Priority` 不同。读 event_socket 输出时要以实际源码为准。

### callback 里不要长期持有 event 指针

`switch_event_deliver()` 回调结束后会销毁 event。callback 如果想异步处理，需要自己复制需要的数据，或者 `switch_event_dup()` 出一份。

## 和我们 demo 的对应关系

| FreeSWITCH | demo | 说明 |
|---|---|---|
| `switch_event_t` | `event_t` | 事件主体。 |
| `switch_event_header_t` | `event_header_t` | header 链表。 |
| `EVENT_NODES[]` | `listeners[]` | 按事件类型分桶的 listener 表。 |
| `switch_event_bind()` | `event_bind()` | 注册 listener。 |
| `switch_event_fire()` | `event_fire()` | 发送事件并转移所有权。 |
| `switch_event_dispatch_thread()` | worker thread | 异步消费队列。 |
| `switch_event_prep_for_delivery()` | `event_prepare_for_delivery()` | 补系统 header。 |
| `CUSTOM_HASH` | subclass registry | custom subclass 注册表。 |

差异：

- FreeSWITCH 当前 dispatch queue 是 FIFO；demo 已增强为按 priority 出队。
- FreeSWITCH callback 原型是 `void callback(switch_event_t *event)`，通过 `event->bind_user_data` 拿绑定上下文；demo callback 原型直接带 `void *user_data`。
- FreeSWITCH header 系统比 demo 更复杂，支持 array、hash、stack 插入语义、变量展开。

## 模拟 Debug 阅读路线

### 场景 1：模块监听所有事件

断点：

1. `mod_event_test.c:126`
2. `switch_event.c:2365`
3. `switch_event.c:2426`

调用链：

```text
mod_event_test_load()
  -> switch_event_bind()
  -> switch_event_bind_removable()
  -> EVENT_NODES[SWITCH_EVENT_ALL] = event_node
```

观察变量：

- `id = modname`
- `event = SWITCH_EVENT_ALL`
- `subclass_name = NULL`
- `event_node->callback = event_handler`

### 场景 2：创建并发送事件

断点：

1. `switch_event.c:817`
2. `switch_event.c:2272`
3. `switch_event.c:2304`
4. `switch_event.c:376`
5. `switch_event.c:313`
6. `switch_event.c:423`

调用链：

```text
switch_event_create()
  -> switch_event_create_subclass_detailed()
  -> switch_event_prep_for_delivery_detailed()
  -> switch_event_fire()
  -> switch_event_fire_detailed()
  -> switch_event_queue_dispatch_event()
  -> switch_event_dispatch_thread()
  -> switch_event_deliver()
```

观察变量：

- `(*event)->event_id`
- `(*event)->headers`
- `EVENT_SEQUENCE_NR`
- `EVENT_DISPATCH_QUEUE` size
- `EVENT_NODES[event_id]`
- `EVENT_NODES[SWITCH_EVENT_ALL]`

### 场景 3：事件匹配 listener

断点：

1. `switch_event.c:423`
2. `switch_event.c:434`
3. `switch_event.c:234`
4. `switch_event.c:439`

观察：

- 第一轮 `e = event->event_id`
- 第二轮 `e = SWITCH_EVENT_ALL`
- `node->subclass_name`
- `event->subclass_name`
- `event->bind_user_data`

## 复习问题

1. 为什么 `switch_event_fire()` 接收 `switch_event_t **`，而不是 `switch_event_t *`？
2. `SWITCH_EVENT_ALL` 为什么必须放在 enum 最后？
3. `switch_event_deliver()` 为什么要先扫具体事件，再扫 `SWITCH_EVENT_ALL`？
4. `switch_event_set_priority()` 是否改变 dispatch 顺序？为什么？
5. 如果 callback 很耗时，为什么应该自己再建队列？
6. `EF_UNIQ_HEADERS` 会怎样影响 header 添加？
7. `switch_event_create_subclass_detailed()` 为什么不允许普通事件带 subclass？
8. `CUSTOM_HASH` 的 `bind` 字段解决了什么问题？
9. `switch_event_destroy()` 为什么也接收二级指针？
10. `event_channel` 和传统 `switch_event_t` 的关系是什么？

## 下一步建议

第一轮只看主链：

```text
switch_event_create_subclass_detailed
switch_event_prep_for_delivery_detailed
switch_event_fire_detailed
switch_event_queue_dispatch_event
switch_event_dispatch_thread
switch_event_deliver
switch_events_match
switch_event_destroy
```

第二轮看 header 系统：

```text
switch_event_base_add_header
switch_event_get_header_ptr
switch_event_del_header_val
switch_event_serialize
```

第三轮再看扩展：

```text
switch_event_expand_headers_check
switch_event_channel_broadcast
switch_live_array_add
```
