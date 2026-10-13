# FreeSWITCH 拨号规则（Dialplan）源码深度解析

> 对照阅读源码指南 — 覆盖从 SIP INVITE 到拨号规则匹配再到 Action 执行的完整调用链

---

## 一、整体调用链一览

```
SIP INVITE 到达
      │
      ▼
sofia_handle_sip_i_invite()          [sofia.c:11353]
  ├─ 解析 SIP 报文，提取被叫号码
  ├─ 创建 caller_profile（填入 destination_number/context/dialplan）
  └─ 触发 switch_channel_set_state(CS_INIT → CS_ROUTING)
                │
                ▼
switch_core_standard_on_routing()    [switch_core_state_machine.c:222]
  ├─ 读取 caller_profile->dialplan（默认 "XML"）
  ├─ 按名称加载拨号引擎接口：switch_loadable_module_get_dialplan_interface("XML")
  └─ 调用 dialplan_interface->hunt_function(session, arg, NULL)
                │
                ▼
dialplan_hunt()                      [mod_dialplan_xml.c:629]
  ├─ 加载 conf/dialplan/*.xml
  ├─ 找到对应 <context>
  ├─ 循环遍历 <extension>，调用 parse_exten()
  └─ 返回匹配到的 switch_caller_extension_t
                │
                ▼
switch_channel_set_state(CS_EXECUTE)
                │
                ▼
switch_core_standard_on_execute()    [switch_core_state_machine.c:318]
  └─ 逐个执行 extension->current_application 链表
     └─ switch_core_session_execute_application()
```

---

## 二、第一层：mod_sofia — SIP 入口

**文件**：`src/mod/endpoints/mod_sofia/sofia.c`

### 2.1 触发点：nua_i_invite 回调

```c
// sofia.c:1888
case nua_i_invite:
    sofia_handle_sip_i_invite(session, nua, profile, nh,
                              sofia_private, sip, de, tags);
```

Sofia-SIP 栈收到 SIP INVITE 消息后，通过 NUA（Sofia-SIP 的高层用户代理 API）
回调触发 `sofia_handle_sip_i_invite()`。

### 2.2 关键代码段（sofia.c:11353 起）

#### ① 提取被叫号码

```c
// sofia.c ~12200 附近
if (sofia_test_pflag(profile, PFLAG_FULL_ID)) {
    destination_number = req_uri;       // 完整 URI 模式
} else {
    destination_number = req_user;      // 默认：只取 user 部分，即被叫号码
}
```

**说明**：`req_user` 来自 SIP Request-URI 的 user 部分（如 `INVITE sip:1001@domain`
中的 `1001`），这就是最终在 dialplan 中匹配的 `destination_number`。

#### ② 创建 caller_profile

```c
// sofia.c:11848
tech_pvt->caller_profile =
    switch_caller_profile_new(switch_core_session_get_pool(session),
                              NULL, NULL, NULL, NULL,
                              NULL, NULL, NULL, NULL,
                              MODNAME, NULL, NULL);
switch_channel_set_caller_profile(channel, tech_pvt->caller_profile);
```

`switch_caller_profile_t` 是贯穿整个呼叫生命周期的核心数据结构，包含：
- `destination_number`：被叫号码
- `caller_id_number`：主叫号码
- `context`：dialplan 上下文名称（如 `"default"`）
- `dialplan`：要使用的拨号引擎名称（如 `"XML"`）

#### ③ 确定 context 和 dialplan

```c
// sofia.c ~12249
// context 优先级：ACL 指定 > 频道变量 user_context > profile 默认
if (!context) {
    context = switch_channel_get_variable(channel, "user_context");
}
if (!context) {
    context = profile->context;   // 通常是 "default"
}

// dialplan 优先级：频道变量 inbound_dialplan > profile 默认
if (!(dialplan = switch_channel_get_variable(channel, "inbound_dialplan"))) {
    dialplan = profile->dialplan;  // 通常是 "XML"（sofia.c:6586 设默认值）
}
```

#### ④ profile 默认 dialplan 来源

```c
// sofia.c:6146 — 读取 sip_profiles/internal.xml 配置
} else if (!strcasecmp(var, "dialplan")) {
    profile->dialplan = switch_core_strdup(profile->pool, val);

// sofia.c:6585 — 配置中未指定时的默认值
if (!profile->dialplan) {
    profile->dialplan = switch_core_strdup(profile->pool, "XML");
}
```

对应的配置文件（`conf/sip_profiles/internal.xml`）：
```xml
<param name="dialplan" value="XML"/>
<param name="context" value="default"/>
```

#### ⑤ 将 caller_profile 填充完整后，触发路由

```c
// sofia.c:12464
profile_dup_clean(dialplan, tech_pvt->caller_profile->dialplan,
                  tech_pvt->caller_profile->pool);
// 之后的状态变迁（CS_INIT -> CS_ROUTING）由 session 线程自动驱动
```

---

## 三、第二层：状态机路由 — switch_core_state_machine

**文件**：`src/switch_core_state_machine.c`

### 3.1 函数签名（第 222 行）

```c
static void switch_core_standard_on_routing(switch_core_session_t *session)
```

### 3.2 核心逻辑逐行解析

```c
// 第 255 行：读取 caller_profile 中的 dialplan 字段（可以逗号分隔多个）
if (!zstr(caller_profile->dialplan)) {
    if ((dpstr = switch_core_session_strdup(session, caller_profile->dialplan))) {
        // 支持 "XML,enum" 这样的多引擎串联
        expanded = switch_channel_expand_variables(session->channel, dpstr);
        argc = switch_separate_string(expanded, ',', dp,
                                       (sizeof(dp) / sizeof(dp[0])));
        for (x = 0; x < argc; x++) {
            char *dpname = dp[x];
            char *dparg = NULL;

            // 支持 "XML:/path/to/alt.xml" 带参数的格式
            if ((dparg = strchr(dpname, ':'))) {
                *dparg++ = '\0';
            }

            // 按名称从模块注册表查找拨号引擎
            if (!(dialplan_interface =
                  switch_loadable_module_get_dialplan_interface(dpname))) {
                switch_log_printf(..., "Dialplan [%s] not found, skipping\n", dpname);
                continue;
            }

            count++;

            // 调用拨号引擎的 hunt_function — 对 XML 引擎就是 dialplan_hunt()
            extension = dialplan_interface->hunt_function(session, dparg, NULL);
            UNPROTECT_INTERFACE(dialplan_interface);

            if (extension) {
                // 匹配成功：将 extension 绑定到 channel，切换到执行状态
                switch_channel_set_caller_extension(session->channel, extension);
                switch_channel_set_state(session->channel, CS_EXECUTE);
                goto end;
            }
        }
    }
}
```

**要点**：
- `dialplan_interface->hunt_function` 是函数指针，由 `mod_dialplan_xml` 注册（第 733 行：`SWITCH_ADD_DIALPLAN(dp_interface, "XML", dialplan_hunt)`）
- 支持多引擎串联：`"XML,enum"` 会先走 XML 失败后再走 ENUM
- 引擎函数返回 NULL 表示未匹配，返回 `switch_caller_extension_t*` 表示匹配成功

---

## 四、第三层：XML 拨号引擎 — mod_dialplan_xml

**文件**：`src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c`

### 4.1 模块注册（第 733 行）

```c
SWITCH_MODULE_LOAD_FUNCTION(mod_dialplan_xml_load)
{
    switch_dialplan_interface_t *dp_interface;
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);
    // 向核心注册名为 "XML" 的拨号引擎，入口函数是 dialplan_hunt
    SWITCH_ADD_DIALPLAN(dp_interface, "XML", dialplan_hunt);
    return SWITCH_STATUS_SUCCESS;
}
```

### 4.2 入口函数：dialplan_hunt（第 629 行）

```c
SWITCH_STANDARD_DIALPLAN(dialplan_hunt)
{
    // 步骤 1：获取 caller_profile（包含 destination_number、context 等）
    if (!caller_profile) {
        caller_profile = switch_channel_get_caller_profile(channel);
    }
    
    // 步骤 2：context 默认值
    if (!caller_profile->context) {
        caller_profile->context = "default";
    }

    // 步骤 3：加载 XML 配置（支持 alt_path 参数指定替代配置文件）
    if (!zstr(alt_path)) {
        alt_root = switch_xml_parse_file_simple(alt_path);
        // 从替代路径加载 dialplan 配置节
    } else {
        dialplan_xml_locate(session, caller_profile, &xml, &cfg);
        // 标准路径：向 XML 绑定层请求 "dialplan" section
        // 会触发 conf/dialplan/*.xml 的加载和合并
    }

    // 步骤 4：找到对应的 <context> 节点
    xcontext = switch_xml_find_child(cfg, "context", "name",
                                     caller_profile->context);

    // 步骤 5：处理 auto_hunt（直接按名称查找 extension，跳过全量遍历）
    if ((hunt = switch_channel_get_variable(channel, "auto_hunt"))
        && switch_true(hunt)) {
        xexten = switch_xml_find_child(xcontext, "extension",
                                        "name", caller_profile->destination_number);
    }

    // 步骤 6：遍历所有 <extension>
    if (!xexten) {
        xexten = switch_xml_child(xcontext, "extension");
    }

    while (xexten) {
        const char *cont = switch_xml_attr(xexten, "continue"); // continue="true" 表示匹配后继续
        proceed = parse_exten(session, caller_profile, xexten,
                              &extension, exten_name, 0);
        if (proceed && !switch_true(cont)) {
            break;  // 默认：匹配到第一个就停止
        }
        xexten = xexten->next;
    }

    return extension;  // 返回给状态机，NULL 表示未匹配
}
```

### 4.3 核心匹配函数：parse_exten（第 98 行）

这是整个拨号规则匹配的核心，处理单个 `<extension>` 内所有的 `<condition>`。

#### 整体结构

```c
static int parse_exten(switch_core_session_t *session,
                       switch_caller_profile_t *caller_profile,
                       switch_xml_t xexten,
                       switch_caller_extension_t **extension,
                       const char *exten_name, int recur)
{
    // 遍历 <extension> 下所有 <condition>
    for (xcond = switch_xml_child(xexten, "condition"); xcond; xcond = xcond->next) {
        // ... 匹配逻辑
    }
}
```

#### ① break 属性解析（第 187 行）

```c
if ((do_break_a = (char *) switch_xml_attr(xcond, "break"))) {
    if (!strcasecmp(do_break_a, "on-true"))  do_break_i = BREAK_ON_TRUE;
    if (!strcasecmp(do_break_a, "on-false")) do_break_i = BREAK_ON_FALSE; // 默认
    if (!strcasecmp(do_break_a, "always"))   do_break_i = BREAK_ALWAYS;
    if (!strcasecmp(do_break_a, "never"))    do_break_i = BREAK_NEVER;
}
```

| break 值 | 含义 |
|----------|------|
| `on-true`（默认） | 条件**匹配**时停止继续检查后续 condition |
| `on-false` | 条件**不匹配**时停止 |
| `always` | 无论是否匹配都停止 |
| `never` | 无论是否匹配都继续 |

#### ② 时间条件检查（第 182 行）

```c
// 检查 <condition> 上的 year/mon/mday/wday/hour/minute 等属性
time_match = switch_xml_std_datetime_check(xcond, tzoff ? &offset : NULL, tzname_);
// time_match = 1: 时间匹配; 0: 时间不匹配; -1: 未指定时间条件（跳过时间检查）
```

#### ③ 正则匹配（第 395 行起，单 condition 场景）

```c
// 获取 field 属性（如 "destination_number"）
field = (char *) switch_xml_attr(xcond, "field");

// 展开 ${变量} 语法
if ((expression_expanded = switch_channel_expand_variables(channel, expression))
    == expression) {
    expression_expanded = NULL;
} else {
    expression = expression_expanded;
}

if (field) {
    // 从 caller_profile 取字段值（如 destination_number = "1001"）
    field_data = switch_caller_get_field_by_name(caller_profile, field);

    // PCRE 正则匹配
    if ((proceed = switch_regex_perform(field_data, expression, &re, &match_data))) {
        anti_action = SWITCH_FALSE; // 匹配成功，执行 <action>
    }
    // 否则 anti_action = SWITCH_TRUE，执行 <anti-action>
} else if (time_match == -1) {
    // 无 field 也无时间条件 → 绝对条件，永远匹配（用于默认路由）
    anti_action = SWITCH_FALSE;
    proceed = 1;
}
```

**`field` 可填的值**（来自 `switch_caller_get_field_by_name`）：
- `destination_number` — 被叫号码（最常用）
- `caller_id_number` — 主叫号码
- `caller_id_name` — 主叫名称
- `network_addr` — 来源 IP
- `ani` / `aniii` — ANI 信息
- `${channel_variable}` — 任意频道变量

#### ④ 多 regex 联合条件（第 230 行）

```c
// <condition regex="all"> 或 <condition regex="any"> 或 <condition regex="xor">
if ((regex_rule = (char *) switch_xml_attr(xcond, "regex"))) {
    int all = !strcasecmp(regex_rule, "all");
    int xor = !strcasecmp(regex_rule, "xor");
    int pass = 0, fail = 0;

    for (xregex = switch_xml_child(xcond, "regex"); xregex; xregex = xregex->next) {
        // 遍历每个子 <regex> 节点，统计 pass/fail 数量
    }

    if (xor)       anti_action = (pass == 1) ? SWITCH_FALSE : SWITCH_TRUE;
    else if (all)  anti_action = (!fail)  ? SWITCH_FALSE : SWITCH_TRUE;
    else           anti_action = (pass)   ? SWITCH_FALSE : SWITCH_TRUE; // any
}
```

对应 XML：
```xml
<condition regex="all">
    <regex field="destination_number" expression="^1\d{3}$"/>
    <regex field="caller_id_number"   expression="^1000$"/>
    <action application="bridge" data="..."/>
</condition>
```

#### ⑤ 正则捕获组变量替换（第 497 行）

```c
if (field && expression && strchr(expression, '(')) {
    // 正则有捕获组时，将捕获内容设为频道变量 DP_MATCH
    switch_channel_set_variable(channel, "DP_MATCH", NULL);
    switch_capture_regex(match_data, proceed, "DP_MATCH",
                         switch_regex_set_var_callback, session);
}

// action 的 data 中可以用 $1/$2 引用捕获组
len = (strlen(data) + strlen(field_data) + 10) * proceed;
substituted = malloc(len);
switch_perform_substitution(match_data, data, substituted, len);
app_data = substituted;
```

示例：
```xml
<condition field="destination_number" expression="^(1\d{3})$">
    <!-- $1 会被替换为实际匹配到的号码 -->
    <action application="bridge" data="user/$1"/>
</condition>
```

#### ⑥ Action 处理（第 494 行）

```c
for (xaction = switch_xml_child(xcond, "action"); xaction; xaction = xaction->next) {
    char *application = switch_xml_attr_soft(xaction, "application");
    char *app_data = ...; // 可能包含 $1/$2 替换后的数据

    const char *inline_ = switch_xml_attr_soft(xaction, "inline");
    int xinline = switch_true(inline_); // inline="true" 则立即执行

    if (xinline) {
        // inline action：在 dialplan 匹配阶段就立即执行
        // 只能执行带 SAF_ROUTING_EXEC 标志的 app（如 set、export 等）
        exec_app(session, application, app_data);
    } else {
        // 普通 action：加入执行队列，CS_EXECUTE 阶段执行
        switch_caller_extension_add_application(session, *extension,
                                                 application, app_data);
    }
}
```

#### ⑦ break 控制流（第 579 行）

```c
// 每个 condition 处理完毕后，根据 break 属性决定是否继续
if (((anti_action == SWITCH_FALSE && do_break_i == BREAK_ON_TRUE) ||
     (anti_action == SWITCH_TRUE  && do_break_i == BREAK_ON_FALSE)) ||
    do_break_i == BREAK_ALWAYS) {
    break;  // 退出 condition 循环
}
```

#### ⑧ 嵌套 condition（递归，第 589 行）

```c
if (proceed) {
    if (switch_xml_child(xcond, "condition")) {
        // <condition> 内嵌套 <condition> 时，递归调用 parse_exten
        if (!(proceed = parse_exten(session, caller_profile, xcond,
                                    extension, orig_exten_name, recur + 1))) {
            if (do_break_i == BREAK_NEVER) continue;
            goto done;
        }
    }
}
```

最大递归深度由 `MAX_RECUR`（通常为 500）限制。

---

## 五、第四层：exec_app — inline action 执行器

**文件**：`src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c:48`

```c
static switch_status_t exec_app(switch_core_session_t *session,
                                const char *app, const char *arg)
{
    switch_application_interface_t *application_interface;

    // 从模块注册表查找 app（如 "set"、"export"）
    if ((application_interface =
         switch_loadable_module_get_application_interface(app)) == 0) {
        // app 不存在 → 挂断
        switch_channel_hangup(channel, SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER);
        return SWITCH_STATUS_FALSE;
    }

    // inline app 必须带 SAF_ROUTING_EXEC 标志，否则报错
    // 这是安全限制，防止在路由阶段执行需要媒体的 app（如 playback）
    if (!switch_test_flag(application_interface, SAF_ROUTING_EXEC)) {
        switch_log_printf(..., "This application cannot be executed inline\n");
        return SWITCH_STATUS_FALSE;
    }

    // 在当前线程同步执行 app
    switch_core_session_exec(session, application_interface, arg);
    return SWITCH_STATUS_SUCCESS;
}
```

**常见支持 inline 执行的 app**：`set`、`export`、`unset`、`log`、`verbose_events` 等。

---

## 六、第五层：CS_EXECUTE — Action 列表执行

**文件**：`src/switch_core_state_machine.c:318`

```c
static void switch_core_standard_on_execute(switch_core_session_t *session)
{
    switch_caller_extension_t *extension;

  top:
    extension = switch_channel_get_caller_extension(session->channel);

    // 遍历 action 链表（extension->current_application 是单链表头指针）
    while (switch_channel_get_state(session->channel) == CS_EXECUTE
           && extension->current_application) {
        switch_caller_application_t *current_application =
            extension->current_application;

        // 移动到下一个 action
        extension->current_application = extension->current_application->next;

        // 执行当前 action（如 bridge、answer、playback 等）
        if (switch_core_session_execute_application(
                session,
                current_application->application_name,
                current_application->application_data) != SWITCH_STATUS_SUCCESS) {
            return; // app 执行失败或信道已挂断
        }
    }

    // 所有 action 执行完毕，挂断呼叫
    if (switch_channel_ready(session->channel) &&
        switch_channel_get_state(session->channel) == CS_EXECUTE) {
        switch_channel_hangup(session->channel, SWITCH_CAUSE_NORMAL_CLEARING);
    }
}
```

---

## 七、数据结构速查

### switch_caller_profile_t（核心呼叫信息）

```c
// src/include/switch_caller.h
struct switch_caller_profile {
    const char *username;
    const char *dialplan;           // 拨号引擎名，如 "XML"
    const char *caller_id_name;
    const char *caller_id_number;
    const char *caller_id_number;
    const char *destination_number; // 被叫号码，dialplan 匹配的主要目标
    const char *context;            // 上下文，如 "default"、"public"
    const char *network_addr;       // 来源 IP
    // ...
};
```

### switch_caller_extension_t（匹配结果）

```c
struct switch_caller_extension {
    const char *extension_name;
    const char *extension_number;
    switch_caller_application_t *applications;      // action 链表头
    switch_caller_application_t *current_application; // 当前执行指针
    // ...
};
```

### switch_caller_application_t（单个 action）

```c
struct switch_caller_application {
    const char *application_name; // 如 "bridge"
    const char *application_data; // 如 "user/1001"
    switch_caller_application_t *next;
};
```

---

## 八、XML 配置与代码对应关系

```xml
<!-- conf/dialplan/default.xml -->
<context name="default">                     <!-- dialplan_hunt: xcontext -->

  <extension name="local_extension">         <!-- dialplan_hunt: xexten, parse_exten: exten_name -->

    <condition field="destination_number"    <!-- parse_exten: field -->
               expression="^(10[0-9]{2})$"  <!-- parse_exten: expression (PCRE 正则) -->
               break="on-false">             <!-- parse_exten: do_break_i = BREAK_ON_FALSE -->

      <!-- 条件匹配后执行，$1 = 捕获组 1 的值 -->
      <action application="set"              <!-- exec_app (inline) 或 add_application -->
              data="call_timeout=30"
              inline="true"/>                <!-- xinline=1 → dialplan 阶段立即执行 -->

      <action application="bridge"           <!-- add_application → CS_EXECUTE 阶段执行 -->
              data="user/$1"/>

      <!-- 条件不匹配时执行 -->
      <anti-action application="hangup"
                   data="NO_ROUTE_DESTINATION"/>
    </condition>

  </extension>

</context>
```

---

## 九、关键源文件速查表

| 文件 | 关键函数 | 职责 |
|------|----------|------|
| `src/mod/endpoints/mod_sofia/sofia.c` | `sofia_handle_sip_i_invite` (L11353) | SIP INVITE 入口，创建 caller_profile，设置 dialplan/context |
| `src/mod/endpoints/mod_sofia/sofia.c` | (L6146, L6585) | sofia profile 默认 dialplan="XML" |
| `src/switch_core_state_machine.c` | `switch_core_standard_on_routing` (L222) | 路由状态机，按名称查找并调用拨号引擎 |
| `src/switch_core_state_machine.c` | `switch_core_standard_on_execute` (L318) | 执行状态机，逐个执行 action 链表 |
| `src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c` | `dialplan_hunt` (L629) | XML 引擎入口，定位 context，遍历 extension |
| `src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c` | `parse_exten` (L98) | 单个 extension 的 condition 匹配逻辑 |
| `src/mod/dialplans/mod_dialplan_xml/mod_dialplan_xml.c` | `exec_app` (L48) | inline action 执行器 |
| `src/include/switch_caller.h` | 数据结构定义 | caller_profile / extension / application |
| `src/include/switch_module_interfaces.h` | `switch_dialplan_interface` (L266) | 拨号引擎接口定义 |
| `conf/dialplan/default.xml` | — | XML 拨号规则配置文件 |
| `conf/sip_profiles/internal.xml` | — | Sofia profile，配置 dialplan/context |

---

## 十、mod_dptools — Action 应用实现

**文件**：`src/mod/applications/mod_dptools/mod_dptools.c`

所有 `<action application="...">` 指定的 app 都在这里注册和实现（少数在其他模块）。

### 10.1 App 注册方式（模块加载时，第 6674 行附近）

```c
// mod_dptools.c:6674
SWITCH_ADD_APP(app_interface,
    "answer",              // XML 中 application= 使用的名称
    "Answer the call",     // 简短描述
    "Answer the call for a channel.",  // 详细描述
    answer_function,       // 实际的函数指针
    "",                    // 参数语法说明
    SAF_SUPPORT_NOMEDIA);  // 标志位

// SAF_ROUTING_EXEC  → 允许在 dialplan 匹配阶段 inline 执行（如 set、export）
// SAF_SUPPORT_NOMEDIA → 支持无媒体模式
// SAF_NONE → 普通 app，只能在 CS_EXECUTE 阶段执行
```

### 10.2 answer（第 1385 行）

```c
SWITCH_STANDARD_APP(answer_function)
{
    switch_channel_t *channel = switch_core_session_get_channel(session);

    // 可选参数：is_conference / decode_video / debug_video
    const char *arg = (char *) data;
    if (zstr(arg)) {
        arg = switch_channel_get_variable(channel, "answer_flags");
    }

    if (!zstr(arg)) {
        if (switch_stristr("is_conference", arg)) {
            switch_channel_set_flag(channel, CF_CONFERENCE);
        }
    }

    // 核心调用：发送 SIP 200 OK，建立媒体
    switch_channel_answer(channel);
    // → switch_core_session_message SWITCH_MESSAGE_INDICATE_ANSWER
    // → mod_sofia 发送 SIP 200 OK + SDP
}
```

### 10.3 playback（第 3029 行）

```c
SWITCH_STANDARD_APP(playback_function)
{
    switch_file_handle_t fh = { 0 };

    // 支持 "file.wav@@8000" 格式（@@ 后指定采样率）
    if (data) {
        file = switch_core_session_strdup(session, data);
        if ((p = strchr(file, '@')) && *(p + 1) == '@') {
            *p = '\0';
            p += 2;
            fh.samples = atoi(p);  // 指定从哪个采样点开始播放（断点续播）
        }
    }

    args.input_callback = on_dtmf;  // DTMF 输入回调（用于打断播放）

    // 核心调用：通过 IVR 层播放文件
    status = switch_ivr_play_file(session, &fh, file, &args);
    // → switch_core_file_open 打开音频文件（支持 wav/mp3/shout 等）
    // → 循环读取音频帧推送到媒体层
}
```

**playback 支持的路径格式**：
- `sounds/en/us/ivr/ivr-please_hold.wav` — 相对路径
- `/absolute/path/to/file.wav` — 绝对路径
- `http://server/file.wav` — HTTP 流（需要 mod_httapi）
- `shout://server/stream` — 流媒体

### 10.4 bridge（第 3480 行）

`bridge` 是最复杂的 app，核心流程分两步：**发起呼叫** + **桥接媒体**。

```c
SWITCH_STANDARD_APP(audio_bridge_function)
{
    switch_core_session_t *peer_session = NULL;
    switch_call_cause_t cause = SWITCH_CAUSE_NORMAL_CLEARING;

    // ── 特殊功能：campon（等待忙线用户空闲）──
    if ((v_campon = switch_channel_get_variable(caller_channel, "campon"))
        && switch_true(v_campon)) {
        // 循环尝试 originate，失败时播放保持音乐
        do {
            status = switch_ivr_originate(session, &peer_session, &cause, data, ...);
        } while (!success && retry--);
    }

    // ── 正常 bridge 流程 ──

    // 步骤 1：发起外呼，建立 peer_session（B 路）
    status = switch_ivr_originate(
        session,         // A 路 session
        &peer_session,   // 输出：新建的 B 路 session
        &cause,          // 输出：失败原因
        data,            // 目标地址（如 "user/1001" 或 "sofia/internal/1001@server"）
        0,               // 超时（0 = 用默认值 60 秒）
        NULL, NULL, NULL, NULL, NULL,
        SOF_NONE, NULL, NULL
    );
    // switch_ivr_originate 内部流程：
    //   1. 解析 endpoint（如 "user/" → mod_sofia, "local/" → mod_loopback）
    //   2. 调用 endpoint->io_routines->outbound_channel_answer()
    //   3. 发送 SIP INVITE 到目标
    //   4. 等待目标应答（SIP 200 OK）

    // 步骤 2：双向桥接音频/视频
    if (status == SWITCH_STATUS_SUCCESS) {
        switch_ivr_multi_threaded_bridge(
            session,       // A 路
            peer_session,  // B 路
            xfer_on_dtmf,  // DTMF 处理回调（可触发转接）
            a_key, b_key   // DTMF 按键配置
        );
        // 内部用两个线程双向转发 RTP 包
        // A → B：读取 A 的音频帧，写入 B
        // B → A：读取 B 的音频帧，写入 A
    }
}
```

#### bridge 目标地址格式

```
user/1001              → 本地注册用户（通过 mod_sofia 的 user/ 端点）
sofia/internal/num@ip  → 直接 SIP URI 外呼
loopback/default/1001  → 内部回环（重新走 dialplan）
group/sales@default    → 呼叫组
```

### 10.5 App 执行底层路径

```
switch_core_standard_on_execute()
  └─ switch_core_session_execute_application("bridge", "user/1001")
       └─ switch_loadable_module_get_application_interface("bridge")
            └─ audio_bridge_function(session, "user/1001")
                 ├─ switch_ivr_originate()      [src/switch_ivr_originate.c]
                 │    └─ endpoint->outbound_channel()  [mod_sofia: sofia_outgoing_channel()]
                 └─ switch_ivr_multi_threaded_bridge()  [src/switch_ivr_bridge.c]
```

### 10.6 常用 App 一览

| XML application | 函数 | 行号 | SAF 标志 | 说明 |
|-----------------|------|------|----------|------|
| `answer` | `answer_function` | 1385 | `SAF_SUPPORT_NOMEDIA` | 应答呼叫，发 200 OK |
| `bridge` | `audio_bridge_function` | 3480 | `SAF_NONE` | 外呼并桥接 |
| `playback` | `playback_function` | 3029 | `SAF_NONE` | 播放音频文件 |
| `set` | `set_function` | ~1300 | `SAF_SUPPORT_NOMEDIA \| SAF_ROUTING_EXEC` | 设置频道变量（支持 inline） |
| `export` | `export_function` | — | `SAF_ROUTING_EXEC` | 导出变量到 B 路 |
| `hangup` | `hangup_function` | — | `SAF_SUPPORT_NOMEDIA` | 挂断呼叫 |
| `transfer` | `transfer_function` | — | `SAF_SUPPORT_NOMEDIA` | 转接到另一个 dialplan 分支 |
| `sleep` | `sleep_function` | — | `SAF_SUPPORT_NOMEDIA` | 等待指定毫秒数 |
| `log` | `log_function` | — | `SAF_SUPPORT_NOMEDIA \| SAF_ROUTING_EXEC` | 写日志（支持 inline） |

---

## 十二、RTP 媒体流处理 — 帧读取、编解码、报文收发

RTP 处理跨越 4 个源文件，构成一条完整的媒体数据管道。

### 12.1 整体数据流

```
┌──────────────────────────────────────────────────────────────────────┐
│  收包方向（入站 RTP → session 帧）                                    │
│                                                                      │
│  UDP Socket                                                          │
│    └─ switch_socket_recvfrom()          [switch_rtp.c:5747]          │
│         └─ rtp_common_read()            [switch_rtp.c:7178]          │
│              ├─ switch_poll() — 等待 socket 可读                      │
│              ├─ read_rtp_packet() — 读取并验证 RTP 头                 │
│              ├─ handle_rfc2833() — DTMF 事件处理                      │
│              ├─ check_jitter() — 抖动计算与统计                       │
│              └─ 返回原始编码数据（如 PCMU/G.722 payload）             │
│         └─ switch_rtp_zerocopy_read_frame() [switch_rtp.c:8066]      │
│              └─ 填充 switch_frame_t（零拷贝：data 指向内部 buf）       │
│         └─ switch_core_media_read_frame() [switch_core_media.c:2846] │
│              └─ 循环直到 datalen > 0                                  │
│    └─ sofia_read_frame()               [mod_sofia.c:68]              │
│         └─ switch_core_session_read_frame() [switch_core_io.c:67]    │
│              ├─ 检查编解码器是否需要转码（need_codec）                 │
│              ├─ switch_core_codec_decode() — 解码为 PCM               │
│              ├─ switch_resample_process() — 重采样（如 8k→16k）       │
│              ├─ media_bug 回调（录音、监听等）                         │
│              └─ 输出 switch_frame_t（PCM 或编码数据）                 │
│                                                                      │
│  发包方向（session 帧 → 出站 RTP）                                    │
│                                                                      │
│  switch_core_session_write_frame()                                   │
│    └─ switch_core_media_write_frame() [switch_core_media.c:3428]     │
│         ├─ 更新时间戳 engine->timestamp_send += samples              │
│         └─ switch_rtp_write_frame()    [switch_rtp.c:8747]           │
│              └─ rtp_common_write()     [switch_rtp.c:8164]           │
│                   ├─ 组装 RTP 头（seq/ts/ssrc/pt）                   │
│                   ├─ SRTP 加密（若启用）                              │
│                   └─ switch_socket_sendto() — 发送 UDP 包            │
└──────────────────────────────────────────────────────────────────────┘
```

### 12.2 rtp_common_read 详解（switch_rtp.c:7178）

这是收包的核心循环，处理所有复杂场景：

```c
static int rtp_common_read(switch_rtp_t *rtp_session, ...) {
    while (switch_rtp_ready(rtp_session)) {

        // ── 定时器模式（音频，固定 20ms 间隔）──
        if (rtp_session->flags[SWITCH_RTP_FLAG_USE_TIMER]) {
            if (rtp_session->jb && !rtp_session->pause_jb) {
                // 有抖动缓冲：尽量多读，缓冲后按节拍输出
                while (switch_poll(rtp_session->read_pollfd, 1, &fdr, 0) == SWITCH_STATUS_SUCCESS) {
                    read_rtp_packet(rtp_session, &bytes, ...);
                }
            }
            // 等待定时器触发（约 20ms 一次）
            switch_core_timer_next(&rtp_session->timer);
        }

        // ── 非定时器模式（视频，event-driven）──
        else {
            poll_status = switch_poll(rtp_session->read_pollfd, 1, &fdr, pt);
            // pt = 5s（正常）或 100ms（视频）或 20ms（有 DTMF）
        }

        // ── 读取实际 RTP 包 ──
        status = read_rtp_packet(rtp_session, &bytes, flags, pmapP, ...);
        //   └─ switch_socket_recvfrom(sock, buf, &bytes)
        //   └─ 验证 RTP 版本号（必须为 2）
        //   └─ 处理 ICE / STUN 包（非 RTP 数据）
        //   └─ 更新 rtp_session->last_rtp_hdr

        // ── 自动 NAT 调整 ──
        if (rtp_session->flags[SWITCH_RTP_FLAG_AUTOADJ]) {
            if (!switch_cmp_addr(rtp_session->rtp_from_addr, rtp_session->remote_addr)) {
                if (++rtp_session->autoadj_tally >= rtp_session->autoadj_threshold) {
                    // 对端 IP/端口变了（NAT 穿越），自动更新远端地址
                    switch_rtp_set_remote_address(rtp_session, tx_host, new_port, ...);
                }
            }
        }

        // ── DTMF 处理（RFC 2833，PT=101）──
        switch (handle_rfc2833(rtp_session, bytes, &do_cng)) {
        case RESULT_GOTO_END:       goto end;       // 纯 DTMF 包，通知上层
        case RESULT_GOTO_RECVFROM: goto recvfrom;  // 继续读
        case RESULT_CONTINUE:       goto result_continue;  // 转为 CNG
        }

        // ── CNG（舒适噪声）──
        if (do_cng) {
            // 生成 CNG 帧（静音包），避免播放端觉察到丢包
            *flags |= SFF_CNG;
            ret = 2 + rtp_header_len;
            goto end;
        }

        break;  // 成功读到 RTP 数据，退出循环
    }
    return (int) bytes;
}
```

### 12.3 switch_core_session_read_frame 的编解码处理（switch_core_io.c:67）

```c
SWITCH_DECLARE(switch_status_t) switch_core_session_read_frame(session, frame, flags, stream_id)
{
    // 步骤 1：调用端点的底层 read_frame（→ sofia_read_frame → switch_rtp_zerocopy_read_frame）
    status = session->endpoint_interface->io_routines->read_frame(session, frame, flags, stream_id);

    // 步骤 2：判断是否需要转码
    if ((*frame)->codec->implementation->codec_id != session->read_codec->implementation->codec_id) {
        need_codec = TRUE;  // 编解码器不匹配，需要转码
    }
    if ((*frame)->rate != session->read_impl.actual_samples_per_second) {
        do_resample = 1;    // 采样率不同，需要重采样
    }

    if (need_codec) {
        // 步骤 3：解码为 PCM
        status = switch_core_codec_decode(
            codec,                // 解码器（如 PCMU → PCM）
            session->read_codec,  // 目标格式
            (*frame)->data,       // 输入：编码数据
            (*frame)->datalen,
            session->read_impl.actual_samples_per_second,
            session->raw_read_buffer,  // 输出：PCM 数据
            &raw_len, &from_rate, &flag
        );

        // 步骤 4：重采样（如对端 8000Hz，本端需要 16000Hz）
        if (do_resample) {
            switch_resample_process(
                session->read_resampler,
                data,          // 输入 PCM
                (int) datalen / 2 / channels
            );
            // 输出已更新到 read_resampler->to 缓冲区
        }

        // 步骤 5：media_bug 拦截（录音、监听、ASR 等使用此机制）
        for (bp = session->bugs; bp; bp = bp->next) {
            if (switch_core_media_bug_test_flag(bp, SMBF_READ_STREAM)) {
                bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_READ);
            }
        }

        // 步骤 6：若需要重新编码（如转码桥接），编码回目标格式
        if (perfect) {
            status = switch_core_codec_encode(
                session->read_codec,
                NULL,
                session->raw_read_buffer,  // PCM 输入
                raw_len,
                session->read_impl.actual_samples_per_second,
                (*frame)->data,            // 编码输出
                &(*frame)->datalen,
                &from_rate, &flag
            );
        }
    }

    // media_bug 对编码数据的拦截（如 spy tap）
    for (bp = session->bugs; bp; bp = bp->next) {
        if (switch_core_media_bug_test_flag(bp, SMBF_READ_REPLACE)) {
            bp->callback(bp, bp->user_data, SWITCH_ABC_TYPE_READ_REPLACE);
        }
    }

    return status;
}
```

### 12.4 关键数据结构

#### switch_frame_t（单个音频/视频帧）

```c
struct switch_frame {
    void *data;         // 帧数据（编码或 PCM）
    uint32_t datalen;   // 数据长度（字节）
    uint32_t buflen;    // 缓冲区总长
    uint32_t samples;   // 样本数（音频）
    uint32_t rate;      // 采样率（如 8000/16000）
    uint32_t channels;  // 声道数
    switch_payload_t payload; // RTP payload type（如 0=PCMU, 8=PCMA）
    uint32_t timestamp; // RTP 时间戳
    uint16_t seq;       // RTP 序列号
    uint32_t ssrc;      // 同步源标识
    switch_bool_t m;    // RTP M 标志位（帧边界）
    switch_frame_flag_t flags; // SFF_CNG / SFF_RFC2833 / SFF_PROXY_PACKET 等
    switch_image_t *img; // 视频帧（libyuv 格式）
};
```

#### switch_rtp_t 中的关键字段

```c
// switch_rtp.c 内部结构（部分）
struct switch_rtp {
    switch_socket_t *sock_input;      // 接收 UDP socket
    switch_socket_t *sock_output;     // 发送 UDP socket
    rtp_msg_t recv_msg;               // 接收缓冲区（含 RTP 头 + payload）
    switch_rtp_hdr_t last_rtp_hdr;    // 最近一个 RTP 包的头

    switch_jb_t *jb;                  // 抖动缓冲（音频 JitterBuffer）
    switch_jb_t *vb;                  // 视频抖动缓冲

    switch_core_timer_t timer;        // 20ms 定时器（音频节拍）
    uint32_t samples_per_interval;    // 每包样本数（如 160 = 8000Hz * 20ms）
    uint32_t ms_per_packet;           // 每包毫秒数（通常 20ms）

    // 统计
    rtp_stats_t stats;                // 包含 jitter/MOS/丢包率等
};
```

### 12.5 媒体数据流与 bridge 的结合

`switch_ivr_multi_threaded_bridge` 开两个线程，每个线程的工作：

```c
// 线程1：A → B 方向
while (channel_alive) {
    switch_core_session_read_frame(a_session, &read_frame, ...);
    // 若 A/B 的编解码器不同，此处自动转码
    switch_core_session_write_frame(b_session, read_frame, ...);
}

// 线程2：B → A 方向（对称）
while (channel_alive) {
    switch_core_session_read_frame(b_session, &read_frame, ...);
    switch_core_session_write_frame(a_session, read_frame, ...);
}
```

如果 A/B 两路使用相同编解码器（如都是 PCMU），则数据是 **零转码直通**（proxy 模式）；
如果不同，核心会自动解码→重采样→重编码，对 app 层透明。

### 12.6 关键源文件

| 文件 | 行号 | 职责 |
|------|------|------|
| [src/switch_rtp.c](src/switch_rtp.c) | 7178 | `rtp_common_read`：底层 UDP 收包、定时器、DTMF、抖动 |
| [src/switch_rtp.c](src/switch_rtp.c) | 8066 | `switch_rtp_zerocopy_read_frame`：零拷贝帧封装 |
| [src/switch_rtp.c](src/switch_rtp.c) | 8164 | `rtp_common_write`：RTP 头组装、SRTP、UDP 发包 |
| [src/switch_core_media.c](src/switch_core_media.c) | 2846 | `switch_core_media_read_frame`：RTP→帧的中间层 |
| [src/switch_core_media.c](src/switch_core_media.c) | 3428 | `switch_core_media_write_frame`：帧→RTP 的中间层 |
| [src/switch_core_io.c](src/switch_core_io.c) | 67 | `switch_core_session_read_frame`：顶层，含编解码和 media_bug |
| [src/switch_ivr_bridge.c](src/switch_ivr_bridge.c) | — | `switch_ivr_multi_threaded_bridge`：双向桥接线程 |

---

## 十一、switch_ivr_originate — 外呼引擎内部解析

**文件**：`src/switch_ivr_originate.c`（共 5048 行）  
**入口函数**：`switch_ivr_originate()`（第 2064 行）

### 11.1 函数签名

```c
SWITCH_DECLARE(switch_status_t) switch_ivr_originate(
    switch_core_session_t *session,           // A 路 session（可为 NULL）
    switch_core_session_t **bleg,             // 输出：新建的 B 路 session
    switch_call_cause_t *cause,               // 输出：失败原因
    const char *bridgeto,                     // 目标拨号字符串
    uint32_t timelimit_sec,                   // 超时（秒，0 = 默认 60 秒）
    const switch_state_handler_table_t *table,
    const char *cid_name_override,
    const char *cid_num_override,
    switch_caller_profile_t *caller_profile_override,
    switch_event_t *ovars,                    // 额外的频道变量
    switch_originate_flag_t flags,
    switch_call_cause_t *cancel_cause,
    switch_dial_handle_t *dh                  // 拨号句柄（新 API）
);
```

### 11.2 拨号字符串格式解析（第 2848 行附近）

`bridge` app 传入的 `data` 参数就是拨号字符串，`switch_ivr_originate` 按如下规则解析：

#### 格式一：简单格式

```
sofia/internal/1001@192.168.1.1
│────────────────│──────────────
chan_type         chan_data
(端点名称)        (传给端点的数据)
```

分割逻辑（第 2909 行）：
```c
if ((chan_data = strchr(chan_type, '/')) != 0) {
    *chan_data = '\0';  // 截断：chan_type = "sofia"（或 "user"、"loopback"）
    chan_data++;        // chan_data = "internal/1001@192.168.1.1"
}
```

#### 格式二：带变量的格式

```
{var1=val1,var2=val2}sofia/internal/1001@ip
└─────────────────┘────────────────────────
  全局变量（{} 语法）    chan_type/chan_data
```

- `{}` 中的变量设置到所有 B 路 session
- `[]` 中的变量只设置到当前这条 B 路（多目标时）

#### 格式三：多目标并行拨号（OR 逻辑）

```
sofia/internal/1001@ip,sofia/internal/1002@ip
```
用逗号分隔，同时发起多路外呼，**任意一路**接通即成功（race）。

#### 格式四：多目标串行拨号（AND 逻辑）

```
sofia/internal/1001@ip:_:sofia/internal/1002@ip
```
用 `:_:` 分隔（`SWITCH_ENT_ORIGINATE_DELIM`），路由到 `switch_ivr_enterprise_originate()`，按顺序尝试。

### 11.3 端点查找与 session 创建（第 3054 行）

```c
// 核心调用：根据 chan_type 名称查找对应的 endpoint 模块
reason = switch_core_session_outgoing_channel(
    oglobals.session,   // A 路 session
    originate_var_event,// 变量事件
    chan_type,          // 端点名称，如 "sofia"、"user"、"loopback"
    new_profile,        // caller_profile
    &new_session,       // 输出：新建的 B 路 session
    NULL,
    myflags,
    cancel_cause
);
```

`switch_core_session_outgoing_channel` 内部流程：
```c
// src/switch_core.c
endpoint_interface = switch_loadable_module_get_endpoint_interface(chan_type);
// 调用端点模块的 outbound_channel 函数
endpoint_interface->io_routines->outbound_channel(session, var_event,
    new_profile, &new_session, NULL, flags, cancel_cause);
```

**常用端点名称 → 模块映射**：

| chan_type | 模块 | 说明 |
|-----------|------|------|
| `sofia` | mod_sofia | 直接 SIP 外呼 |
| `user` | mod_sofia | 本地注册用户（内部查目录再转 sofia） |
| `loopback` | mod_loopback | 内部回环，重新走 dialplan |
| `freetdm` | mod_freetdm | TDM/PSTN 线路 |
| `opal` | mod_opal | H.323 / T.38 |

### 11.4 等待应答 + 状态轮询（第 470 行 `check_channel_status`）

```c
// 外呼发出后，轮询 B 路的状态
while (elapsed < timelimit_sec) {
    switch (check_channel_status(oglobals, ...)) {
    case 1:  // 已接通（200 OK）
        goto end;
    case -1: // 超时 / 被拒绝
        goto fail;
    }
    switch_yield(1000 * 1000 / 100);  // 睡 10ms 后继续轮询
}
```

轮询中同时处理：
- A 路的 RTP 回铃音（ringback）播放
- B 路的 180 Ringing / 183 Session Progress
- 超时控制（`progress_timeout` / `call_timeout`）

### 11.5 桥接音频（第 3675 行）

```c
// B 路接通后，进入桥接
switch_ivr_multi_threaded_bridge(
    session,      // A 路
    peer_session, // B 路
    func,         // DTMF 回调（可用于盲转/出席转）
    a_key,        // A 路触发 DTMF 按键
    b_key         // B 路触发 DTMF 按键
);
// 内部开两个线程：
//   线程1: A→B（switch_core_session_read_frame + switch_core_session_write_frame）
//   线程2: B→A（同上，方向相反）
```

### 11.6 完整调用链（bridge 端到端）

```
<action application="bridge" data="user/1001"/>
  │
  ├─ 解析 chan_type="user", chan_data="1001"
  │
  ├─ switch_core_session_outgoing_channel("user", ...)
  │    └─ mod_sofia: sofia_outgoing_channel("user/1001")
  │         └─ 查目录 conf/directory/default/1001.xml
  │              └─ 获取注册地址 → 发 SIP INVITE
  │
  ├─ 等待 SIP 200 OK（轮询 check_channel_status）
  │
  └─ switch_ivr_multi_threaded_bridge(A路, B路)
       ├─ 线程1: 读 A帧 → 写 B
       └─ 线程2: 读 B帧 → 写 A
```

---

## 十三、ICE/STUN NAT 穿越处理

**文件**：`src/switch_rtp.c`（ICE/STUN 实现）+ `src/switch_core_media.c`（ICE 激活）

### 13.1 背景：为什么需要 ICE？

```
场景：SIP 设备在 NAT 后面

  ┌──────────────────────────┐      ┌──────────────────────────┐
  │ SIP Client A             │      │ FreeSWITCH               │
  │ 私有 IP: 192.168.1.100  │      │ 公网 IP: 203.0.113.1    │
  │ NAT 公网: 1.2.3.4:10000 │      │ RTP 端口: 16384          │
  └──────────────────────────┘      └──────────────────────────┘
            │                                 │
            └─── SDP 中携带 ICE 候选者 ────────┘
                 a=ice-ufrag:xxx
                 a=ice-pwd:yyy
                 a=candidate:0 1 UDP 2130706431 192.168.1.100 5004 typ host
                 a=candidate:1 1 UDP 1694498815 1.2.3.4 10000 typ srflx
                                              ↑ NAT 映射的外网地址（server reflexive）
```

ICE（Interactive Connectivity Establishment，RFC 8445）通过 STUN 连通性检查，
找到双方均可到达的候选地址对，解决 NAT 穿越问题。

### 13.2 ICE 类型枚举（src/include/switch_types.h:2869）

```c
typedef enum {
    ICE_VANILLA   = (1 << 1),  // 标准 ICE（RFC 8445），需要连通性检查
    ICE_CONTROLLED = (1 << 2), // 受控角色（被叫方，服从对端的 USE-CANDIDATE 选择）
    ICE_LITE      = (1 << 3),  // ICE-Lite（简化版，不主动发 Binding Request）
} switch_core_media_ice_type_t;
// ICE_CONTROLLED + ICE_LITE = 被叫方轻量 ICE（WebRTC SFU 常见配置）
```

### 13.3 ICE 激活流程

#### ① SDP 协商后激活 ICE（switch_core_media.c:4093）

```c
static switch_core_media_ice_type_t switch_determine_ice_type(engine, session) {
    switch_core_media_ice_type_t ice_type = ICE_VANILLA; // 默认标准 ICE

    if (switch_channel_var_true(session->channel, "ice_lite")) {
        ice_type |= ICE_CONTROLLED;
        ice_type |= ICE_LITE;     // 显式配置 ICE-Lite 模式
    } else {
        direction = switch_ice_direction(engine, session);
        if (direction == SWITCH_CALL_DIRECTION_INBOUND) {
            ice_type |= ICE_CONTROLLED; // 被叫方 = 受控角色
        }
        // 主叫方 = 控制角色（ICE-CONTROLLING，默认不加此 flag）
    }
    return ice_type;
}

// switch_core_media.c:4572 — 激活 ICE
switch_rtp_activate_ice(
    engine->rtp_session,
    engine->ice_in.ufrag,    // 本端 ufrag
    engine->ice_in.rip,      // 对端 ufrag
    engine->ice_in.pass,     // 本端密码
    engine->ice_in.rpass,    // 对端密码（用于 HMAC-SHA1 验证）
    IPR_RTP,                 // RTP 路
    ice_type,
    &engine->ice_in          // 候选者列表
);
```

#### ② ICE 凭证格式（switch_rtp.c:4955）

```c
// ice_user = "本端ufrag:对端ufrag"（发出的 Binding Request 中的 USERNAME 属性）
switch_snprintf(ice_user, sizeof(ice_user), "%s:%s", login, rlogin);
// user_ice = "对端ufrag:本端ufrag"（收到的 Binding Request 中期望的 USERNAME 属性）
switch_snprintf(user_ice, sizeof(user_ice), "%s:%s", rlogin, login);

ice->ready  = 0;  // 本端是否收到过对端的 Binding Request
ice->rready = 0;  // 对端是否已确认本端的 Binding Request（收到 Binding Response）
```

### 13.4 连通性检查：主动发 Binding Request（ice_out，第 857 行）

```c
static switch_status_t ice_out(switch_rtp_t *rtp_session, switch_rtp_ice_t *ice, switch_bool_t force)
{
    // ICE-Lite 模式不主动发（只响应对端的检查）
    if (ice->type & ICE_LITE) {
        return SWITCH_STATUS_BREAK;
    }

    // 频率控制：1 秒发一次（RTP_STUN_FREQ = 1000000 微秒）
    if (!force && ice->next_run && ice->next_run >= now) {
        return SWITCH_STATUS_BREAK;
    }
    ice->next_run = now + RTP_STUN_FREQ;

    // 构造 STUN Binding Request 包
    packet = switch_stun_packet_build_header(SWITCH_STUN_BINDING_REQUEST, NULL, buf);
    switch_stun_packet_attribute_add_username(packet, ice->ice_user, strlen(ice->ice_user));

    if (ice->type & ICE_VANILLA) {
        // 标准 ICE 属性
        switch_stun_packet_attribute_add_priority(packet,
            ice->ice_params->cands[chosen][proto].priority); // 候选者优先级

        if (ice->type & ICE_CONTROLLED) {
            switch_stun_packet_attribute_add_controlled(packet);   // ICE-CONTROLLED 属性
        } else {
            switch_stun_packet_attribute_add_controlling(packet);  // ICE-CONTROLLING 属性
            switch_stun_packet_attribute_add_use_candidate(packet); // 声明使用此候选对
        }

        // 消息完整性（HMAC-SHA1，使用对端密码）
        switch_stun_packet_attribute_add_integrity(packet, ice->rpass);
        // 指纹（CRC32）
        switch_stun_packet_attribute_add_fingerprint(packet);
    }

    // 发送到当前选中的候选者地址
    switch_socket_sendto(sock_output, ice->addr, 0, (void *) packet, &bytes);
}
```

### 13.5 处理收到的 STUN 包（handle_ice，第 956 行）

在 `rtp_common_read` 的 `read_rtp_packet` 中，若收到的 UDP 包版本号非 RTP（v=2），
会被识别为 STUN/ICE 包并交给 `handle_ice` 处理：

```c
static void handle_ice(switch_rtp_t *rtp_session, switch_rtp_ice_t *ice,
                        void *data, switch_size_t len)
{
    packet = switch_stun_packet_parse(buf, cpylen);  // 解析 STUN 包

    // ── 收到 Binding Response（对端回复了我们的 Binding Request）──
    if (packet->header.type == SWITCH_STUN_BINDING_RESPONSE) {
        if (ice->sending) ice->sending--;
        if (!ice->rready) {
            ice->rready = 1;  // 对端可达！标记 rready
        }
    }

    // ── 收到 Binding Request（对端主动发起连通性检查）──
    if (packet->header.type == SWITCH_STUN_BINDING_REQUEST) {
        if (!ice->ready) {
            ice->ready = 1;  // 本端收到对端请求，标记 ready
        }

        // 验证 USERNAME（必须匹配 "对端ufrag:本端ufrag"）
        // 回复 Binding Response（包含对端的 XOR-MAPPED-ADDRESS）
        rpacket = switch_stun_packet_build_header(SWITCH_STUN_BINDING_RESPONSE, packet->header.id, stunbuf);
        switch_stun_packet_attribute_add_xor_binded_address(rpacket, remote_ip, from_port, family);
        switch_socket_sendto(sock_output, from_addr, 0, (void *) rpacket, &bytes);

        // ── 候选者切换逻辑（do_adj）──
        // 若收到的包来自非当前选中候选者地址，根据以下条件决定是否切换：
        // 1. STUN 超时超过 3 秒（elapsed_stun >= 3000）
        // 2. 媒体中断超过 2 秒（elapsed_media >= MEDIA_TOO_LONG）
        // 3. DTLS 尚未就绪
        // 4. 收到 USE-CANDIDATE 属性（ICE 控制方指定此候选对）
        if (do_adj) {
            switch_rtp_change_ice_dest(rtp_session, ice, from_host, from_port);
            ice->rready = 1;
            ice->initializing = 0;
        }
    }
}
```

### 13.6 ICE 状态完成条件

```
ice->ready  = 1 AND ice->rready = 1
     │                    │
     │                    └─ 收到了对端对我们 Binding Request 的 Response
     └─ 我们收到了对端的 Binding Request（并回复了 Response）

两个条件都满足 → ICE 连通性建立，RTP 媒体流开始传输
```

### 13.7 候选者类型及优先级

| 候选者类型 | 来源 | 典型场景 |
|-----------|------|----------|
| `host` | 本地网卡地址 | 同局域网通话（最高优先级） |
| `srflx`（server reflexive） | STUN 服务器反射的公网 IP:Port | NAT 后的设备，需要 STUN |
| `relay` | TURN 服务器中继地址 | 严格 NAT 或防火墙（最低优先级，兜底） |

选择算法：优先使用 `host`，其次 `srflx`，最后才是 `relay`（成本最高）。

### 13.8 完整 ICE 握手时序

```
FreeSWITCH (控制方)              SIP 客户端 (受控方)
      │                                │
      │──── SIP INVITE (含 SDP) ──────→│
      │     a=ice-ufrag:FS_ufrag       │
      │     a=ice-pwd:FS_pwd           │
      │     a=candidate:... typ host   │
      │                                │
      │←─── SIP 200 OK (含 SDP) ───────│
      │     a=ice-ufrag:CLI_ufrag      │
      │     a=ice-pwd:CLI_pwd          │
      │     a=candidate:... typ srflx  │
      │                                │
      │── STUN Binding Request ───────→│  (ice_out: 每 1 秒发一次)
      │   USERNAME: "FS_ufrag:CLI_ufrag"│
      │   ICE-CONTROLLING              │
      │   USE-CANDIDATE                │
      │   INTEGRITY (HMAC-SHA1)        │
      │                                │
      │←─ STUN Binding Request ────────│  (对端发起检查)
      │   USERNAME: "CLI_ufrag:FS_ufrag"│
      │                                │
      │── STUN Binding Response ──────→│  (handle_ice: 回复对端)
      │   XOR-MAPPED-ADDRESS           │
      │                                │
      │←─ STUN Binding Response ───────│  (ice->rready = 1)
      │                                │
      │   ice->ready=1, ice->rready=1  │
      │      → ICE 连通性建立！         │
      │                                │
      │═══════════ RTP 媒体流 ═══════════│  (正式传输音频/视频)
```

### 13.9 关键源文件

| 文件 | 行号 | 职责 |
|------|------|------|
| [src/switch_rtp.c](src/switch_rtp.c#L857) | 857 | `ice_out`：主动发送 STUN Binding Request |
| [src/switch_rtp.c](src/switch_rtp.c#L956) | 956 | `handle_ice`：处理收到的 STUN 包，候选者切换 |
| [src/switch_rtp.c](src/switch_rtp.c#L4932) | 4932 | `switch_rtp_activate_ice`：初始化 ICE 凭证 |
| [src/switch_core_media.c](src/switch_core_media.c#L4093) | 4093 | `switch_determine_ice_type`：确定 ICE 角色 |
| [src/switch_core_media.c](src/switch_core_media.c#L4572) | 4572 | SDP 协商后调用 `switch_rtp_activate_ice` |
| [src/switch_stun.c](src/switch_stun.c) | — | STUN 包构造/解析工具函数 |
| [src/include/switch_stun.h](src/include/switch_stun.h) | — | STUN 数据结构定义 |

---

## 十四、WebRTC 客户端接入完整流程（WebSocket + ICE + DTLS-SRTP）

FreeSWITCH 通过 **mod_verto** 模块实现 WebRTC 客户端接入。
Verto 协议 = WebSocket 传输 + JSON-RPC 2.0 信令 + WebRTC 媒体（ICE + DTLS-SRTP）。

### 14.1 架构总览

```
┌────────────────────────────────────────────────────────────────────┐
│  浏览器 WebRTC 客户端                                               │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │  JavaScript SDK (verto.js)                                   │  │
│  │  WebSocket (wss://fs.example.com:8082)                      │  │
│  │  RTCPeerConnection (ICE + DTLS + SRTP)                      │  │
│  └──────────────────────────────────────────────────────────────┘  │
└──────────────────────────┬─────────────────────────────────────────┘
                           │ WebSocket
                           ▼
┌────────────────────────────────────────────────────────────────────┐
│  FreeSWITCH mod_verto (src/mod/endpoints/mod_verto/mod_verto.c)   │
│                                                                    │
│  jsock（WebSocket 连接对象）                                       │
│  JSON-RPC 2.0 消息处理（process_jrpc）                            │
│  Verto 方法：verto.invite / verto.answer / verto.bye 等           │
│                                                                    │
│  → 转换为 switch_core_session_t                                   │
│  → 走 dialplan → CS_EXECUTE → bridge / conference                 │
└──────────────────────────┬─────────────────────────────────────────┘
                           │ UDP/DTLS-SRTP
                           ▼
                      RTP 媒体传输
```

### 14.2 WebRTC 信令：JSON-RPC 2.0 消息格式

Verto 协议的所有信令通过 WebSocket 发送 JSON-RPC 2.0 消息。

#### 主要方法注册（mod_verto.c:4634）

```c
jrpc_add_func("login",             login_func);           // 登录认证
jrpc_add_func("verto.invite",      verto__invite_func);   // WebRTC 客户端发起呼叫
jrpc_add_func("verto.answer",      verto__answer_func);   // WebRTC 客户端应答
jrpc_add_func("verto.bye",         verto__bye_func);      // 挂断
jrpc_add_func("verto.attach",      verto__attach_func);   // 重新附加（呼叫恢复）
jrpc_add_func("verto.modify",      verto__modify_func);   // 修改（保持/取消保持）
jrpc_add_func("verto.subscribe",   verto__subscribe_func);// 订阅事件
jrpc_add_func("verto.mediaParams", ...);                   // 更新媒体参数
```

#### 典型消息示例

**浏览器 → FreeSWITCH（发起呼叫）**：
```json
{
  "jsonrpc": "2.0",
  "method": "verto.invite",
  "id": 1,
  "params": {
    "sessid": "abc123",
    "dialogParams": {
      "callID": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
      "destination_number": "1001",
      "caller_id_name": "Alice",
      "caller_id_number": "1000"
    },
    "sdp": "v=0\r\no=- 123 1 IN IP4 192.168.1.100\r\n..."
  }
}
```

**FreeSWITCH → 浏览器（回复 SDP）**：
```json
{
  "jsonrpc": "2.0",
  "method": "verto.media",
  "params": {
    "callID": "xxxxxxxx-...",
    "sdp": "v=0\r\no=FreeSWITCH 123 2 IN IP4 203.0.113.1\r\n..."
  }
}
```

### 14.3 verto.invite 处理流程（mod_verto.c:3960）

```c
static switch_bool_t verto__invite_func(const char *method, cJSON *params,
                                         jsock_t *jsock, cJSON **response)
{
    // 步骤 1：从 JSON 中提取关键字段
    call_id = cJSON_GetObjectCstr(dialog, "callID");
    sdp     = cJSON_GetObjectCstr(params, "sdp");  // 浏览器的 WebRTC SDP Offer
    destination_number = cJSON_GetObjectCstr(dialog, "destination_number");

    // 步骤 2：创建 rtc endpoint 的 session
    reason = switch_core_session_outgoing_channel(NULL, var_event,
        "rtc",          // 端点类型（WebRTC 专用端点）
        NULL, &session, NULL, SOF_NONE, &cancel_cause);

    // 步骤 3：设置 tech_pvt（Verto 私有数据）
    tech_pvt->r_sdp = switch_core_session_strdup(session, sdp);
    switch_core_media_set_sdp_codec_string(session, sdp, SDP_OFFER); // 解析浏览器 SDP

    // 步骤 4：设置媒体选项（ICE、DTLS-SRTP 启用）
    verto_set_media_options(tech_pvt, jsock->profile);
    // 内部会 switch_channel_set_flag(channel, CF_DTLS)
    // 内部会 switch_channel_set_flag(channel, CF_ICE)

    // 步骤 5：创建 caller_profile（含 dialplan/context）
    caller_profile = switch_caller_profile_new(pool,
        jsock->uid,
        jsock->profile->dialplan,    // 从 verto profile 获取，通常 "XML"
        caller_id_name, caller_id_number,
        jsock->remote_host,          // WebSocket 来源 IP
        ...
        context,                     // 上下文
        destination_number           // 被叫号码
    );
    switch_channel_set_caller_profile(channel, caller_profile);

    // 步骤 6：启动 session 线程，走 dialplan 路由
    switch_channel_set_state(channel, CS_INIT);
    switch_core_session_thread_launch(session);
    // → CS_INIT → CS_ROUTING → dialplan → CS_EXECUTE → ...
}
```

### 14.4 WebRTC SDP 与 ICE/DTLS 协商

WebRTC 的 SDP 与普通 SIP SDP 的关键区别：

```
# 普通 SIP SDP（RTP）
m=audio 5004 RTP/AVP 0 8
a=rtpmap:0 PCMU/8000

# WebRTC SDP（SRTP over ICE）
m=audio 9 UDP/TLS/RTP/SAVPF 111 103 104 9 0 8 106 105 13 110 112 113 126
a=ice-ufrag:xxxx
a=ice-pwd:xxxxxxxxxxxxxxxxxxxxxxxx
a=fingerprint:sha-256 AA:BB:CC:...  ← DTLS 证书指纹
a=setup:actpass                     ← DTLS 角色：可以是 client 或 server
a=rtcp-mux                         ← RTP 和 RTCP 复用同一端口
a=candidate:0 1 UDP 2130706431 192.168.1.100 54321 typ host
a=candidate:1 1 UDP 1694498815 1.2.3.4 54321 typ srflx raddr ...
```

FreeSWITCH 在 `switch_core_media_activate_rtp()` 中处理这些差异：

```c
// switch_core_media.c
// 1. 解析 SDP 中的 ICE 候选者列表
// 2. 根据 a=fingerprint 和 a=setup 初始化 DTLS
// 3. 调用 switch_rtp_activate_ice(...)  激活 ICE 连通性检查
// 4. 调用 switch_rtp_add_dtls(...)      激活 DTLS 握手
```

### 14.5 DTLS 握手过程

```
FreeSWITCH (DTLS Server/Client)      浏览器 (DTLS Client/Server)
         │                                   │
         │── ICE Binding Request ───────────→│
         │←─ ICE Binding Response ───────────│
         │   (ICE 连通性确认)                 │
         │                                   │
         │── DTLS ClientHello ──────────────→│  (或由浏览器先发，取决于 a=setup)
         │←─ DTLS ServerHello + Certificate ─│
         │── DTLS Certificate ──────────────→│
         │   (含 FreeSWITCH 自签名证书)       │
         │←─ DTLS Finished ─────────────────│
         │── DTLS Finished ──────────────────→│
         │   ── DTLS 握手完成 ──              │
         │   协商出 SRTP Master Key           │
         │                                   │
         │══════ SRTP 加密媒体流 ══════════════│
```

FreeSWITCH 的 DTLS 证书配置（`conf/ssl/` 下）：
```c
// switch_core_media.c:4043
switch_core_cert_gen_fingerprint(DTLS_SRTP_FNAME, &engine->local_dtls_fingerprint);
// 生成或加载 conf/ssl/dtls-srtp.pem
```

### 14.6 完整呼叫建立时序

```
浏览器                         mod_verto               FreeSWITCH 核心
  │                               │                           │
  │─ WebSocket connect ──────────→│                           │
  │                               │                           │
  │─ login {"method":"login"} ───→│                           │
  │←─ {"result":{"message":"OK"}}─│                           │
  │                               │                           │
  │─ verto.invite {sdp: offer} ──→│                           │
  │   (WebRTC SDP Offer)          │                           │
  │                               │─ create session ─────────→│
  │                               │  dial plan: "XML"         │
  │                               │  context: "default"       │
  │                               │  destination: "1001"      │
  │                               │                           │
  │                               │←─ CS_ROUTING ─────────────│
  │                               │   dialplan_hunt()         │
  │                               │   匹配 extension 1001     │
  │                               │                           │
  │                               │←─ CS_EXECUTE ─────────────│
  │                               │   bridge "user/1001"      │
  │                               │                           │
  │                               │←─ SDP answer (本端) ──────│
  │                               │   switch_core_media_      │
  │                               │   activate_rtp()          │
  │                               │                           │
  │←─ verto.media {sdp: answer} ──│  (含 ICE 候选者和 DTLS 指纹)
  │   (WebRTC SDP Answer)         │                           │
  │                               │                           │
  ├════ ICE 连通性检查 ═══════════════════════════════════════│
  │                               │                           │
  ├════ DTLS 握手 ════════════════════════════════════════════│
  │                               │                           │
  ├════ SRTP 媒体流 ══════════════════════════════════════════│
  │                               │                           │
  │─ verto.bye {} ───────────────→│                           │
  │                               │─ switch_channel_hangup ──→│
  │←─ WebSocket close ────────────│                           │
```

### 14.7 verto.answer 流程（被叫方应答）

当 FreeSWITCH 向浏览器发起呼叫时，浏览器接听后发 `verto.answer`：

```c
// mod_verto.c:2920 — verto__answer_func
static switch_bool_t verto__answer_func(...) {
    // 收到浏览器的 SDP Answer
    tech_pvt->r_sdp = switch_core_session_strdup(session, sdp);
    switch_core_media_set_sdp_codec_string(session, sdp, SDP_ANSWER);

    // 进行编解码器协商
    verto_tech_media(tech_pvt, tech_pvt->r_sdp, SDP_ANSWER);

    // 激活 RTP 传输（包含 ICE + DTLS-SRTP 激活）
    switch_core_media_activate_rtp(tech_pvt->session);
    //   └─ 解析 SDP 中的 ICE 候选者
    //   └─ switch_rtp_activate_ice(...)  发起 ICE 连通性检查
    //   └─ 若有 a=fingerprint，启动 DTLS 握手
    //   └─ DTLS 完成后，SRTP 密钥就绪，媒体开始传输

    // 标记为已应答
    switch_channel_mark_answered(tech_pvt->channel);
}
```

### 14.8 关键源文件

| 文件 | 行号 | 职责 |
|------|------|------|
| [src/mod/endpoints/mod_verto/mod_verto.c](src/mod/endpoints/mod_verto/mod_verto.c#L3960) | 3960 | `verto__invite_func`：处理 WebRTC 呼叫发起 |
| [src/mod/endpoints/mod_verto/mod_verto.c](src/mod/endpoints/mod_verto/mod_verto.c#L2920) | 2920 | `verto__answer_func`：处理浏览器应答 |
| [src/mod/endpoints/mod_verto/mod_verto.c](src/mod/endpoints/mod_verto/mod_verto.c#L1445) | 1445 | `process_jrpc`：JSON-RPC 2.0 消息分发路由 |
| [src/switch_core_media.c](src/switch_core_media.c#L4093) | 4093 | ICE 类型确定（Controlled/Controlling/Lite） |
| [src/switch_core_media.c](src/switch_core_media.c#L4572) | 4572 | `switch_rtp_activate_ice` 调用（SDP 协商后） |
| [src/switch_rtp.c](src/switch_rtp.c#L4932) | 4932 | `switch_rtp_activate_ice`：ICE 凭证初始化 |
| [src/switch_rtp.c](src/switch_rtp.c#L857) | 857 | `ice_out`：STUN Binding Request 发送 |
| [src/switch_rtp.c](src/switch_rtp.c#L956) | 956 | `handle_ice`：STUN 响应处理、候选者切换 |
| `conf/verto.conf.xml` | — | Verto profile 配置（端口、SSL 证书路径） |
| `conf/ssl/dtls-srtp.pem` | — | DTLS-SRTP 证书（自动生成） |

---

## 十五、协议层级完整对照图

```
┌─────────────────────────────────────────────────────────────────────────┐
│  应用层                                                                  │
│  ┌───────────────────┐  ┌─────────────────────────────────────────────┐ │
│  │  SIP Endpoint     │  │  WebRTC Browser (mod_verto)                 │ │
│  │  (mod_sofia)      │  │  JSON-RPC 2.0 over WebSocket               │ │
│  └─────────┬─────────┘  └──────────────────┬──────────────────────────┘ │
│            │ SIP/SDP                        │ Verto/SDP                  │
│            ▼                                ▼                           │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  FreeSWITCH 核心 (switch_core_state_machine)                     │   │
│  │  CS_ROUTING → dialplan_hunt → parse_exten → CS_EXECUTE           │   │
│  │                                                                  │   │
│  │  mod_dptools: bridge → switch_ivr_originate                      │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  媒体层                                                                  │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  switch_core_session_read_frame / write_frame                    │   │
│  │  ├─ switch_core_codec_decode / encode（编解码转换）               │   │
│  │  ├─ switch_resample_process（采样率转换）                         │   │
│  │  └─ media_bug 拦截（录音/监听/ASR）                              │   │
│  └──────────────────────────────────────────────────────────────────┘   │
│                                                                          │
│  RTP/SRTP 传输层                                                         │
│  ┌──────────────────────────────────────────────────────────────────┐   │
│  │  switch_rtp_zerocopy_read_frame / switch_rtp_write_frame         │   │
│  │  ├─ ICE 连通性检查（STUN Binding Request/Response）               │   │
│  │  ├─ DTLS 握手（仅 WebRTC，协商 SRTP 密钥）                       │   │
│  │  ├─ SRTP 加解密（仅 WebRTC）                                     │   │
│  │  └─ UDP 收发（switch_socket_recvfrom / sendto）                  │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```
