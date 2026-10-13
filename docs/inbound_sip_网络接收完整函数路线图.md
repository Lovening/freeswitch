# FreeSWITCH 入方向 SIP — 从网络收到数据到 Session 状态机的完整函数路线图

> 适用版本：FreeSWITCH 1.10.13-dev git 8af8f19 2026-04-21 64bit  
> 主要模块：`mod_sofia` / `switch_core_session` / `switch_core_state_machine`  
> 文档目的：帮助跟读与排查 **入方向呼叫（Inbound SIP INVITE）** 的完整代码路径

---

## 一、整体分层架构

```
┌──────────────────────────────────────────────────────────────────┐
│  Layer 1  操作系统 / Sofia-SIP 内部（FreeSWITCH 无源码覆盖）      │
│           UDP/TCP Socket  →  tport 传输层  →  nua 事件循环       │
└──────────────────────────────────────────────────────────────────┘
                    │ nua 触发注册的回调函数
                    ▼
┌──────────────────────────────────────────────────────────────────┐
│  Layer 2  mod_sofia 回调入口 + 事件封装入队                        │
│           sofia_event_callback()  →  sofia_queue_message()       │
└──────────────────────────────────────────────────────────────────┘
                    │ switch_queue_push → msg_queue
                    ▼
┌──────────────────────────────────────────────────────────────────┐
│  Layer 3  消息队列处理线程池                                       │
│           sofia_msg_thread_run()  →  sofia_process_dispatch_event│
└──────────────────────────────────────────────────────────────────┘
                    │ our_sofia_event_callback()
                    ▼
┌──────────────────────────────────────────────────────────────────┐
│  Layer 4  INVITE 处理 + Session / Channel 建立                    │
│           sofia_handle_sip_i_invite()  →  switch_core_session_   │
│           request()  →  sofia_glue_attach_private()              │
└──────────────────────────────────────────────────────────────────┘
                    │ switch_core_session_thread_launch()
                    ▼
┌──────────────────────────────────────────────────────────────────┐
│  Layer 5  Session 状态机线程                                       │
│           CS_INIT → CS_ROUTING → CS_EXECUTE → CS_HANGUP          │
└──────────────────────────────────────────────────────────────────┘
```

---

## 二、详细代码路线图（含精确行号）

### Layer 1 — 操作系统 / Sofia-SIP 栈（无 FS 源码）

```
OS: socket recv()
      │
      ▼
Sofia-SIP tport 传输层
      │  重组 SIP 报文（UDP/TCP/TLS）
      ▼
Sofia-SIP nua 层
      │  根据 SIP Method（INVITE/BYE/REGISTER/OPTIONS…）
      │  生成对应的 nua_event_t
      │
      │  ← 以下两处是 nua 回调注册点（profile 启动时写入）：
      │    sofia.c : L3467  nua_create(…, sofia_event_callback, …)
      │    sofia.c : L3636  （第二处注册，覆盖部分 profile）
      ▼
触发 FreeSWITCH 注册的 C 函数指针 sofia_event_callback
```

---

### Layer 2 — 回调入口与事件入队

**文件：** `src/mod/endpoints/mod_sofia/sofia.c`

```c
// ─── 入口：nua 栈线程调用（此时还在 Sofia-SIP 内部线程上）────────
// sofia.c : L2512
void sofia_event_callback(nua_event_t event, int status, char const *phrase,
                          nua_t *nua, sofia_profile_t *profile,
                          nua_handle_t *nh, sofia_private_t *sofia_private,
                          sip_t const *sip, tagi_t tags[])
{
    sofia_dispatch_event_t *de;
    int critical = (((SOFIA_MSG_QUEUE_SIZE * mod_sofia_globals.max_msg_queues) * 900) / 1000);
    uint32_t sess_count = switch_core_session_count();
    uint32_t sess_max   = switch_core_session_limit(0);

    // ── 快速过载检测 ──────────────────────────────────────────
    // 当 sess_count >= sess_max 时对 nua_i_invite 直接 503 拒绝
    switch (event) {
        case nua_i_invite:
        case nua_i_register:
        case nua_i_options:
        case nua_i_notify:
        case nua_i_info:
            /* 如果 session 数超限，直接在栈线程里发 503 并返回 */
            if (sess_count >= sess_max || ...) {
                nua_respond(nh, 503, "Maximum Calls In Progress", ...);
                goto end;
            }
            break;
        ...
    }

    // ── 分配 sofia_dispatch_event_t ──────────────────────────
    // 这是一个轻量容器，封装了 nua 事件所有信息
    de = su_alloc(nua_handle_get_home(nh), sizeof(*de));
    ...
    de->event   = nua_save_event(nua, de->data);  // 深拷贝事件数据
    de->nh      = nua_handle_ref_user(nh);
    de->nua     = nua_ref_user(nua);
    de->profile = profile;
    de->sip     = sip_object(de->data->e_msg);    // 解析好的 SIP 结构体

    // ── 新入方向 INVITE：在此处立即创建 session（特殊路径）──
    // （参见 L2627 段详细说明）

    // ── 普通路径：将 de 送入队列 ─────────────────────────────
    sofia_queue_message(de);   // sofia.c : L2474
end:
    ;
}
```

```c
// sofia.c : L2474
void sofia_queue_message(sofia_dispatch_event_t *de)
{
    // 若已关闭则直接在当前线程处理
    if (mod_sofia_globals.running == 0 || !mod_sofia_globals.msg_queue) {
        sofia_process_dispatch_event(&de);
        return;
    }

    // PFLAG_THREAD_PER_REG：REGISTER 消息独立线程处理
    if (de->profile && sofia_test_pflag(de->profile, PFLAG_THREAD_PER_REG) &&
        de->data->e_event == nua_i_register &&
        DE_THREAD_CNT < mod_sofia_globals.max_reg_threads) {
        sofia_process_dispatch_event_in_thread(&de);   // 独立线程
        return;
    }

    // 队列压力过大时动态扩容处理线程
    if (switch_queue_size(mod_sofia_globals.msg_queue) >
        (SOFIA_MSG_QUEUE_SIZE * (unsigned int)msg_queue_threads)) {
        sofia_msg_thread_start(mod_sofia_globals.msg_queue_len + 1);
    }

    // ── 核心：压入全局消息队列 ────────────────────────────────
    switch_queue_push(mod_sofia_globals.msg_queue, de);  // L2500
}
```

> **关键点**：`sofia_event_callback` 在 Sofia-SIP 的 nua 内部线程被调用，
> 为了不阻塞 nua 栈，立即把事件封装后 push 到 `msg_queue`，由独立线程池消费。

---

### 新入方向 INVITE 的 Session 提前创建（重要特殊路径）

**文件：** `src/mod/endpoints/mod_sofia/sofia.c`，约 L2627 段

当 `event == nua_i_invite && sofia_private == NULL`（第一个 INVITE，无关联 session）时，
FreeSWITCH **在 sofia_event_callback 栈线程里** 就创建好 session，
然后再把事件入队。这样 Worker 线程消费时 session 已存在。

```c
// sofia.c : L2627 （位于 sofia_event_callback 内）
if (event == nua_i_invite && !sofia_private) {
    // 1. 申请新 session（包含 pool、channel 等所有基础结构）
    session = switch_core_session_request(sofia_endpoint_interface,
                                          SWITCH_CALL_DIRECTION_INBOUND,
                                          SOF_NONE, NULL);           // L2698

    if (session) {
        // 2. 分配 tech_pvt（SIP 私有数据）
        tech_pvt = sofia_glue_new_pvt(session);                      // sofia_glue.c : L56

        // 3. 从 SIP From/Contact/Referred-By 提取 channel_name
        //    url_set_chanvars 同时设置 sip_from_user / sip_from_host 等变量
        if (sip->sip_from) {
            channel_name = url_set_chanvars(session, sip->sip_from->a_url, sip_from);  // L2710
        }
        if (!channel_name && sip->sip_contact) {
            channel_name = url_set_chanvars(session, sip->sip_contact->m_url, sip_contact);
        }
        if (sip->sip_referred_by) {
            channel_name = url_set_chanvars(session, sip->sip_referred_by->b_url, sip_referred_by);
        }

        // 4. 绑定 tech_pvt 到 session，初始化 channel 名称
        sofia_glue_attach_private(session, profile, tech_pvt, channel_name);  // sofia_glue.c : L76
        //    内部调用 sofia_glue_set_name() → switch_channel_set_name()
        //    → switch_channel.c : L1143  NOTICE "New Channel sofia/internal/6010008 [uuid]"

        // 5. 保存 SIP Call-ID 到 tech_pvt 和 channel 变量
        set_call_id(tech_pvt, sip);                                  // L2718
    }

    // 6. 启动 session 状态机线程（独立线程，开始 CS_NEW 等待）
    switch_core_session_thread_launch(session);                      // L2735
    //    → switch_core_session.c : L1947

    // 7. 保存 uuid 到 sofia_private，供 Worker 线程后续使用
    switch_copy_string(sofia_private->uuid_str, switch_core_session_get_uuid(session), ...);
    de->init_session = session;
}
```

---

### Layer 3 — 消息队列处理线程池

**文件：** `src/mod/endpoints/mod_sofia/sofia.c`

```c
// sofia.c : L2404
// 消费线程：模块初始化时创建，数量由 max-msg-queues 配置
void *SWITCH_THREAD_FUNC sofia_msg_thread_run(switch_thread_t *thread, void *obj)
{
    void *pop;
    switch_queue_t *q = (switch_queue_t *)obj;

    for (;;) {
        // 阻塞等待，直到队列有消息
        if (switch_queue_pop(q, &pop) != SWITCH_STATUS_SUCCESS) {
            switch_cond_next();
            continue;
        }

        if (pop) {
            sofia_dispatch_event_t *de = (sofia_dispatch_event_t *)pop;
            sofia_process_dispatch_event(&de);   // L2428
        } else {
            break;  // NULL 哨兵，线程退出
        }
    }
    return NULL;
}
```

```c
// sofia.c : L2377
void sofia_process_dispatch_event(sofia_dispatch_event_t **dep)
{
    sofia_dispatch_event_t *de = *dep;
    nua_handle_t  *nh      = de->nh;
    nua_t         *nua     = de->nua;
    sofia_profile_t *profile = de->profile;

    // 从 nua_handle 魔法指针取 sofia_private（含 session uuid）
    sofia_private_t *sofia_private = nua_handle_magic(de->nh);
    *dep = NULL;

    // ── 核心分发 ─────────────────────────────────────────────
    our_sofia_event_callback(                                        // L2385
        de->data->e_event, de->data->e_status, de->data->e_phrase,
        de->nua, de->profile, de->nh,
        sofia_private, de->sip, de,
        (tagi_t *)de->data->e_tags
    );

    // 清理
    nua_destroy_event(de->event);
    su_free(nua_handle_get_home(nh), de);

    profile->queued_events--;
    nua_handle_unref_user(nh);
    nua_unref_user(nua);
}
```

---

### Layer 4A — 事件分发核心（our_sofia_event_callback）

**文件：** `src/mod/endpoints/mod_sofia/sofia.c : L1547`

```c
// sofia.c : L1547
// 注释原文：// sofia_event_callback->our_sofia_event_callback
static void our_sofia_event_callback(
    nua_event_t event, int status, char const *phrase,
    nua_t *nua, sofia_profile_t *profile, nua_handle_t *nh,
    sofia_private_t *sofia_private, sip_t const *sip,
    sofia_dispatch_event_t *de, tagi_t tags[])
{
    switch_core_session_t *session = NULL;
    // ... 变量声明

    // ── 从 sofia_private 定位 session ───────────────────────
    if (sofia_private && !zstr(sofia_private->uuid)) {
        if ((session = de->init_session)) {
            de->init_session = NULL;           // 取出预创建的 session
        } else {
            session = switch_core_session_locate(sofia_private->uuid);
        }
        tech_pvt = switch_core_session_get_private(session);
        channel  = switch_core_session_get_channel(session);
        switch_mutex_lock(tech_pvt->sofia_mutex);
    }

    // ── 全局鉴权检查（PFLAG_AUTH_ALL）───────────────────────
    // ...

    // ── 大 switch 事件分发 ───────────────────────────────────
    switch (event) {
        case nua_r_invite:                                            // L1854
            sofia_handle_sip_r_invite(...);   // 出方向 INVITE 响应
            break;

        case nua_i_invite:                                            // L1888
            if (sofia_private->is_call > 1) {
                sofia_handle_sip_i_reinvite(...);   // Re-INVITE（已建立通话中的重协商）
            } else {
                sofia_private->is_call++;
                sofia_handle_sip_i_invite(...);     // 初始 INVITE 处理  ← 主路径
            }
            break;

        case nua_i_state:                                             // L1908
            sofia_handle_sip_i_state(...);    // SIP 对话状态变更（180/200/ACK 等）
            break;

        case nua_i_bye:                                               // L1866
            sofia_handle_sip_i_bye(...);      // 收到 BYE，挂机
            break;

        case nua_i_register:
            sofia_reg_handle_sip_i_register(...);   // 注册处理
            break;

        case nua_i_options:
            sofia_handle_sip_i_options(...);
            break;

        case nua_i_cancel:
            // 设置 hangup 变量，让 CS_ROUTING 路由失败处理
            break;

        // ... 其他事件
    }

done:
    // 解锁、释放资源、可能销毁 nh
    if (locked && tech_pvt) switch_mutex_unlock(tech_pvt->sofia_mutex);
    if (session && session != de->session) switch_core_session_rwunlock(session);
}
```

---

### Layer 4B — 初始 INVITE 处理

**文件：** `src/mod/endpoints/mod_sofia/sofia.c : L11353`

```c
// sofia.c : L11353
void sofia_handle_sip_i_invite(switch_core_session_t *session, nua_t *nua,
                               sofia_profile_t *profile, nua_handle_t *nh,
                               sofia_private_t *sofia_private, sip_t const *sip,
                               sofia_dispatch_event_t *de, tagi_t tags[])
{
    // ── 变量声明（大量 SIP 字段临时变量）───────────────────────
    char network_ip[80] = "";
    int  network_port   = 0;
    const char *r_sdp   = NULL;
    // ...

    // ── 基本校验 ─────────────────────────────────────────────
    if (!session || sess_count >= sess_max || !sofia_test_pflag(profile, PFLAG_RUNNING)) {
        nua_respond(nh, 503, "Maximum Calls In Progress", ...);
        goto fail;
    }

    // ── 获取来源 IP:Port ────────────────────────────────────
    sofia_glue_get_addr(de->data->e_msg, network_ip, sizeof(network_ip), &network_port);

    // ── 提取 SDP（如果 INVITE 携带了 SDP）─────────────────
    if (sip->sip_payload && sip->sip_payload->pl_data) {
        r_sdp = sip->sip_payload->pl_data;
    }

    // ── ACL 检查（IP 白名单）────────────────────────────────
    // profile->acl_count 配置的 ACL 列表逐一检查
    // 检查失败 → nua_respond 403 / 401

    // ── SIP 鉴权 ─────────────────────────────────────────────
    // sofia_reg_parse_auth() → 验证 Authorization / Proxy-Authorization 头
    // 通过 is_auth = 1

    // ── 解析 From/To/Contact/Request-URI 等 header ──────────
    // 设置大量 channel 变量（sip_from_user、sip_to_user 等）

    // ── 创建 caller_profile（主叫信息结构体）────────────────
    // switch_caller_profile_new(session, ...)

    // ── 关联 SDP 到 tech_pvt ────────────────────────────────
    // tech_pvt->mparams.remote_sdp_str = r_sdp

    // ── 切换状态到 CS_INIT，让 session 线程开始工作 ──────────
    switch_channel_set_state(channel, CS_INIT);
    // → 唤醒 switch_core_session_run() 中阻塞的 session 线程
}
```

---

### Layer 5 — Session 状态机线程

**文件：** `src/switch_core_session.c` + `src/switch_core_state_machine.c` + `src/mod/endpoints/mod_sofia/mod_sofia.c`

#### 线程入口

```c
// switch_core_session.c : L1947
SWITCH_DECLARE(switch_status_t) switch_core_session_thread_launch(switch_core_session_t *session)
{
    // 从线程池取出一个工作线程，或新建线程
    // 执行函数为 switch_core_session_thread
    switch_thread_create(&thread, thd_attr, switch_core_session_thread, session, session->pool);
}

// switch_core_session.c : L1721
static void *SWITCH_THREAD_FUNC switch_core_session_thread(switch_thread_t *thread, void *obj)
{
    switch_core_session_t *session = obj;
    session->thread = thread;

    switch_core_session_run(session);           // 进入状态机主循环

    switch_core_media_bug_remove_all(session);  // 清理媒体 bug
    // ... soft lock 等待外部实体
    switch_core_session_destroy(&session);       // 最终销毁 session
    return NULL;
}
```

#### 状态机主循环

```c
// switch_core_state_machine.c : L613
SWITCH_DECLARE(void) switch_core_session_run(switch_core_session_t *session)
{
    switch_channel_state_t state = CS_NEW;

    switch_mutex_lock(session->mutex);

    // ── 状态机主循环 ─────────────────────────────────────────
    while ((state = switch_channel_get_state(session->channel)) != CS_DESTROY) {

        if (state != switch_channel_get_running_state(session->channel)) {
            // 状态变化：依次调用三层处理器
            // 1. endpoint driver handler  (sofia 的 on_xxx)
            // 2. channel event handler    (应用层注册的)
            // 3. 默认 core 行为

            switch_channel_set_running_state(session->channel, state);

            switch (state) {

                case CS_NEW:     // L683  刚创建，等待第一条指令
                    // 仅打印日志，等待状态变更
                    break;

                case CS_INIT:    // L705  基本初始化
                    STATE_MACRO(init, "INIT");
                    // → sofia_on_init()         mod_sofia.c : L92
                    // → switch_event CHANNEL_CREATE 事件
                    // → 出方向：sofia_glue_do_invite() 发送 SIP INVITE
                    // → 入方向：不做额外动作，等 sofia_handle_sip_i_state 推进
                    break;

                case CS_ROUTING: // L724  拨号计划查找
                    STATE_MACRO(routing, "ROUTING");
                    // → sofia_on_routing()      mod_sofia.c : L152
                    //      入方向发 100 Trying（PFLAG_AUTO_INVITE_100）
                    // → switch_core_state_machine_on_routing()
                    //      调用 switch_ivr_parse_all_events() + dialplan 查找
                    //      执行完返回 extension
                    break;

                case CS_EXECUTE: // L731  执行拨号计划应用
                    STATE_MACRO(execute, "EXECUTE");
                    // → sofia_on_execute()      mod_sofia.c : L209
                    // → switch_core_session_execute_application()
                    //      逐个执行 dialplan app：answer、bridge、playback 等
                    break;

                case CS_HANGUP:  // L699  挂机
                    switch_core_session_hangup_state(session, SWITCH_TRUE);
                    switch_channel_set_state(session->channel, CS_REPORTING);
                    break;

                case CS_REPORTING:  // CDR 生成
                    switch_core_session_reporting_state(session);
                    switch_channel_set_state(session->channel, CS_DESTROY);
                    break;

                case CS_DESTROY:
                    goto done;
            }
        }

        // 同一状态下等待唤醒（条件变量 sleep）
        if (endstate == switch_channel_get_running_state(session->channel)) {
            switch_channel_state_thread_lock(session->channel);
            switch_ivr_parse_all_events(session);
            // 若状态未变化，线程睡眠等待外部 switch_channel_set_state() 唤醒
            switch_thread_cond_wait(session->cond, session->mutex);
            switch_channel_state_thread_unlock(session->channel);
        }
    }
done:
    switch_mutex_unlock(session->mutex);
}
```

#### Sofia 各状态回调实现

```c
// mod_sofia.c : L92
static switch_status_t sofia_on_init(switch_core_session_t *session)
{
    // 入方向：仅做 dtmf 类型检查、proxy_mode SDP 吸收
    // 出方向：调用 sofia_glue_do_invite() → 发送 SIP INVITE 请求
    if (sofia_test_flag(tech_pvt, TFLAG_OUTBOUND)) {
        sofia_glue_do_invite(session);   // 出方向在此发 INVITE
    }
    return SWITCH_STATUS_SUCCESS;
}

// mod_sofia.c : L152
static switch_status_t sofia_on_routing(switch_core_session_t *session)
{
    // 入方向：如果配置了 PFLAG_AUTO_INVITE_100，发送 100 Trying
    if (sofia_test_pflag(tech_pvt->profile, PFLAG_AUTO_INVITE_100) && ...) {
        sofia_acknowledge_call(session);
    }
    return SWITCH_STATUS_SUCCESS;
}

// mod_sofia.c : L209
static switch_status_t sofia_on_execute(switch_core_session_t *session)
{
    // 清理 SIP hold 标志，打印日志
    return SWITCH_STATUS_SUCCESS;
}
```

---

## 三、完整时序图

```
 Sofia-SIP nua 线程                 FreeSWITCH Worker 线程             Session 线程
─────────────────────────          ──────────────────────────         ─────────────────
OS recv() SIP INVITE
      │
      ▼
sofia_event_callback()
  sofia.c : L2512
      │
      ├── 超载检查（503）
      │
      ├── 分配 sofia_dispatch_event_t
      │
      ├── 新 INVITE（no sofia_private）─────────────────────────────► switch_core_session_request()
      │     switch_core_session_request()                                    │
      │     sofia_glue_new_pvt()                                      sofia_glue_new_pvt()
      │     url_set_chanvars()                                         url_set_chanvars()
      │     sofia_glue_attach_private()                                sofia_glue_attach_private()
      │     sofia_glue_set_name()                                      sofia_glue_set_name()
      │     → switch_channel_set_name()                             switch_channel_set_name()
      │       L1143 NOTICE "New Channel"                              L1143 NOTICE "New Channel"
      │     set_call_id()                                             set_call_id()
      │     switch_core_session_thread_launch() ──────────────────────────────────────────────────►
      │                                                                                        switch_core_session_thread()
      │                                                                                        switch_core_session_run()
      │                                                                                        状态：CS_NEW (等待)
      │
      ▼
sofia_queue_message()
  sofia.c : L2474
      │
      ▼
switch_queue_push(msg_queue, de)
  sofia.c : L2500
      │
      │ ──────────────────────────►
                                  sofia_msg_thread_run()
                                    sofia.c : L2404
                                        │
                                    switch_queue_pop()
                                        │
                                    sofia_process_dispatch_event()
                                      sofia.c : L2377
                                        │
                                    our_sofia_event_callback()
                                      sofia.c : L1547
                                        │
                                        └── case nua_i_invite: (L1888)
                                              │
                                              ▼
                                        sofia_handle_sip_i_invite()
                                          sofia.c : L11353
                                              │
                                              ├── 鉴权 / ACL 检查
                                              ├── 解析 SIP 头字段
                                              ├── 设置 channel 变量
                                              ├── 创建 caller_profile
                                              ├── 关联 SDP
                                              │
                                              └── switch_channel_set_state(CS_INIT)
                                                    │
                                                    │ 唤醒 session 线程
                                                    │ ──────────────────────────────────────────────►
                                                                                              CS_INIT
                                                                                              sofia_on_init()
                                                                                              mod_sofia.c : L92
                                                                                                │
                                                                                              CS_ROUTING
                                                                                              sofia_on_routing()
                                                                                              mod_sofia.c : L152
                                                                                                │
                                                                                              dialplan 查找
                                                                                                │
                                                                                              CS_EXECUTE
                                                                                              sofia_on_execute()
                                                                                              mod_sofia.c : L209
                                                                                                │
                                                                                              执行 app
                                                                                              (answer/bridge/…)
                                                                                                │
                                                                                              CS_HANGUP
                                                                                              → 发 BYE
                                                                                                │
                                                                                              CS_REPORTING
                                                                                              → CDR
                                                                                                │
                                                                                              CS_DESTROY
```

---

## 四、关键函数速查表

| 阶段 | 函数名 | 文件 | 行号 | 说明 |
|------|--------|------|------|------|
| nua 回调注册 | `nua_create(…sofia_event_callback…)` | sofia.c | L3467, L3636 | profile 启动时注册 |
| **网络接收入口** | `sofia_event_callback()` | sofia.c | **L2512** | nua 栈线程调用 |
| Session 创建 | `switch_core_session_request()` | sofia.c | L2698 | 分配 session + pool |
| tech_pvt 分配 | `sofia_glue_new_pvt()` | sofia_glue.c | L56 | 分配 private_object_t |
| 提取 channel_name | `url_set_chanvars()` | sofia.c | L2710 | 从 SIP From URL 提取 |
| tech_pvt 绑定 | `sofia_glue_attach_private()` | sofia_glue.c | L76 | 绑定 profile/channel |
| channel 命名 | `sofia_glue_set_name()` | sofia_glue.c | L64 | 格式化为 `sofia/internal/6010008` |
| **Channel 日志** | `switch_channel_set_name()` | switch_channel.c | **L1143** | NOTICE "New Channel" |
| 事件入队 | `sofia_queue_message()` | sofia.c | L2474 | 压入 msg_queue |
| 入队操作 | `switch_queue_push()` | sofia.c | L2500 | 实际 push |
| **Worker 线程** | `sofia_msg_thread_run()` | sofia.c | **L2404** | 消费 msg_queue |
| 事件分发 | `sofia_process_dispatch_event()` | sofia.c | L2377 | 调用 our_sofia_event_callback |
| **事件大switch** | `our_sofia_event_callback()` | sofia.c | **L1547** | 所有 SIP 事件分发 |
| **INVITE 处理** | `sofia_handle_sip_i_invite()` | sofia.c | **L11353** | 鉴权/路由/SDP |
| Session 线程启动 | `switch_core_session_thread_launch()` | switch_core_session.c | L1947 | 启动独立线程 |
| Session 线程函数 | `switch_core_session_thread()` | switch_core_session.c | L1721 | 线程入口 |
| **状态机主循环** | `switch_core_session_run()` | switch_core_state_machine.c | **L613** | 状态机 while 循环 |
| CS_INIT 回调 | `sofia_on_init()` | mod_sofia.c | L92 | 出方向发 INVITE |
| CS_ROUTING 回调 | `sofia_on_routing()` | mod_sofia.c | L152 | 发 100 Trying / 路由 |
| CS_EXECUTE 回调 | `sofia_on_execute()` | mod_sofia.c | L209 | 执行 dialplan app |

---

## 五、线程模型总结

```
┌───────────────────────────────────────────────────────────────┐
│  线程名称              │  数量    │  职责                       │
├───────────────────────────────────────────────────────────────┤
│  Sofia nua 栈线程      │  1/profile│  运行 su_root_run()         │
│                        │          │  接收所有 SIP 事件           │
│                        │          │  调用 sofia_event_callback   │
├───────────────────────────────────────────────────────────────┤
│  sofia_msg_thread_run  │  1~N     │  消费 msg_queue              │
│  (Worker 线程池)        │  动态扩展│  处理 SIP 事件              │
│                        │          │  调用 our_sofia_event_callback│
├───────────────────────────────────────────────────────────────┤
│  switch_core_session   │  每路通话│  运行 switch_core_session_run│
│  _thread               │  1个线程 │  驱动 CS_INIT→ROUTING→      │
│                        │          │  EXECUTE→HANGUP 状态机       │
└───────────────────────────────────────────────────────────────┘
```

---

## 六、跟读调试建议

### 6.1 在 fs_cli 中观察入方向呼叫

```bash
/usr/local/freeswitch/bin/fs_cli -P 8121

# 开启 SIP 调试
freeswitch> sofia profile internal siptrace on

# 观察 channel 创建日志
freeswitch> console loglevel debug

# 发起一个入方向呼叫，关注以下日志关键字：
# "New Channel sofia/internal/XXXX"     → switch_channel.c:L1143
# "SOFIA INIT"                          → mod_sofia.c:L92
# "SOFIA ROUTING"                       → mod_sofia.c:L152
# "SOFIA EXECUTE"                       → mod_sofia.c:L209
```

### 6.2 关键断点建议（gdb）

```gdb
# 新 channel 建立
b switch_channel.c:1143

# nua 事件入口
b sofia.c:2512

# INVITE 处理入口
b sofia.c:11353

# 状态机主循环入口
b switch_core_state_machine.c:613

# CS_INIT 切换
b mod_sofia.c:92
```

### 6.3 常见排查路径

| 现象 | 怀疑位置 |
|------|----------|
| 日志无 "New Channel" | `sofia.c:L2627` session 创建失败，检查 sess_max |
| 403/401 鉴权失败 | `sofia.c:L11353` 鉴权检查段，检查 ACL / auth |
| INVITE 后无路由 | `CS_ROUTING` 状态，检查 dialplan context |
| 媒体不通 | `CS_EXECUTE` 阶段 bridge/answer，检查 RTP |
| 通话秒挂 | `sofia_handle_sip_i_state` 中的 i_state 处理 |

---

*文档生成时间：2026-06-30*  
*覆盖源码范围：sofia.c / sofia_glue.c / mod_sofia.c / switch_core_session.c / switch_core_state_machine.c / switch_channel.c*
