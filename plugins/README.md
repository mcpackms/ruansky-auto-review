# JS 插件系统开发指南

## 目录

1. [概述](#1-概述)
2. [快速开始](#2-快速开始)
3. [插件配置](#3-插件配置)
4. [插件生命周期](#4-插件生命周期)
5. [完整 Hook 参考](#5-完整-hook-参考)
6. [完整 API 参考](#6-完整-api-参考)
7. [KV 存储](#7-kv-存储)
8. [文件系统访问](#8-文件系统访问)
9. [HTTP 请求](#9-http-请求)
10. [编码工具](#10-编码工具)
11. [安全与沙箱](#11-安全与沙箱)
12. [调试与日志](#12-调试与日志)
13. [最佳实践](#13-最佳实践)
14. [完整示例](#14-完整示例)

---

## 1. 概述

auto_review 支持使用 JavaScript 编写插件，通过嵌入的 QuickJS-ng 引擎（ES2020 标准）运行。每个插件运行在独立的 QuickJS Runtime + Context 中，互不干扰。

插件通过 `module.exports` 导出 hook 函数，在审核流程的关键节点被调用。插件可以：

- 在文本检查前/后注入自定义规则
- 监控审核事件（通过/拒绝/错误）
- 记录审核统计
- 发送 HTTP 通知（Webhook）
- 读写文件（限制在插件数据目录内）
- 使用 KV 存储持久化数据

---

## 2. 快速开始

### 目录结构

```
plugins/
  data/              # 插件数据目录（自动创建）
    <plugin_name>/   # 每个插件一个子目录，用于文件 I/O
  examples/          # 示例插件
  my_plugin.js       # 你的插件
  README.md          # 本文件
```

### 最简单的插件

创建 `plugins/hello.js`：

```javascript
module.exports = {
    name: "hello",
    version: "1.0.0",
    description: "我的第一个插件",

    onLoad(config) {
        log("info", "Hello 插件已加载！");
        print("插件配置:", JSON.stringify(config));
    },

    onReviewRoundEnd(familyId, total, approved, rejected) {
        log("info", `家族 ${familyId} 本轮完成: ${total} 项`);
    }
};
```

### 启用插件

在 `settings.toml` 中注册：

```toml
[PLUGINS]
dirs = ["./plugins"]

[[PLUGINS.LOAD]]
name = "hello"
file = "hello.js"
enabled = true
[PLUGINS.LOAD.config]
```

---

## 3. 插件配置

### settings.toml 格式

```toml
[PLUGINS]
# 插件搜索目录列表，会扫描其中所有 .js 文件并自动加载
dirs = ["./plugins", "./plugins/examples"]

# 手动配置单个插件（可选，覆盖自动加载的同名插件）
[[PLUGINS.LOAD]]
name = "my_plugin"          # 插件名称（必填，与自动扫描同名时覆盖）
file = "my_plugin.js"       # JS 文件路径，相对于 dirs（与 inline 二选一）
inline = ""                 # 内联 JS 代码（与 file 二选一）
enabled = true              # 是否启用
allow_exec = false          # 是否允许 exec() 系统命令 API（默认禁用）

# 插件自定义配置（JS 中通过 onLoad(config) 接收）
[PLUGINS.LOAD.config]
key1 = "value1"
key2 = 42
key3 = true
```

### 加载顺序

1. **Phase 1**: 处理 `[[PLUGINS.LOAD]]` 手动配置项
2. **Phase 2**: 扫描所有 `dirs` 目录，自动加载未加载的 `.js` 文件

同名插件以手动配置为准（自动扫描会跳过已加载的名称）。

### 配置优先级

- `file` 与 `inline` 互斥，同时指定时 `file` 优先
- `enabled = false` 的插件会被跳过加载
- `allow_exec = true` 需谨慎使用，仅在信任的插件中开启

---

## 4. 插件生命周期

```
启动时:
  1. 解析 settings.toml，读取 [PLUGINS] 配置
  2. 对每个 enabled 的插件:
     a. 创建独立 JSRuntime + JSContext
     b. 注册桥接 API（40+ 个全局函数）
     c. 应用沙箱限制（删除 eval/Function/WebAssembly）
     d. 读取 data 目录（plugins/data/<name>/）
     e. 执行 JS 代码，提取 module.exports
     f. 缓存 hook 函数引用
     g. 调用 onLoad(config)
  3. 插件加载完成，开始审核循环

运行时:
  - 在审核流程的关键节点分发事件到所有已加载插件
  - 同步 hook（onBeforeCheck, onAfterCheck）串行执行
  - 异步 hook（其余事件）以 fire-and-forget 方式触发

停止时:
  - 自动释放所有 JSRuntime + JSContext
  - KV 存储自动持久化（写入 data/<name>/store.json）
```

---

## 5. 完整 Hook 参考

所有 hook 均在 `module.exports` 对象上定义，可选实现。

### 5.1 onLoad(config)

插件加载时调用一次。

```javascript
onLoad(config) {
    // config: 插件配置对象（来自 settings.toml [PLUGINS.LOAD.config]）
    // this: 插件实例（可在 this 上存储状态）
}
```

### 5.2 onBeforeCheck(text, familyId, type)

文本检查前调用。**必须同步**。可以覆盖默认检查结果。

```javascript
onBeforeCheck(text, familyId, type) {
    // text:     待检查的原始文本
    // familyId: 家族 ID
    // type:     内容类型（"post" | "comment" | "up" | "up_resource"）
    //
    // 返回值:
    //   null / undefined    -> 继续默认检查
    //   { reject: true, reason: "..." }   -> 直接拒绝
    //   { reject: false }                 -> 直接通过
}
```

### 5.3 onAfterCheck(text, result, familyId, type)

文本检查后调用。**必须同步**。可以修改检查结果。

```javascript
onAfterCheck(text, result, familyId, type) {
    // text:     待检查的原始文本
    // result:   默认检查结果
    //   { reject: bool, reason: string }
    // familyId: 家族 ID
    // type:     内容类型
    //
    // 返回值:
    //   null / undefined    -> 保持原结果不变
    //   { reject: true, reason: "..." }   -> 覆盖为拒绝
    //   { reject: false }                 -> 覆盖为通过
}
```

### 5.4 onItemApproved(familyId, type, itemId)

项目通过时调用。

```javascript
onItemApproved(familyId, type, itemId) {
    // familyId: 家族 ID
    // type:     内容类型
    // itemId:   项目 ID
}
```

### 5.5 onItemRejected(familyId, type, itemId, reason)

项目被拒绝时调用。

```javascript
onItemRejected(familyId, type, itemId, reason) {
    // familyId: 家族 ID
    // type:     内容类型
    // itemId:   项目 ID
    // reason:   拒绝原因
}
```

### 5.6 onReviewRoundStart(familyId)

每轮审核开始前调用。

```javascript
onReviewRoundStart(familyId) {
    // familyId: 家族 ID
}
```

### 5.7 onReviewRoundEnd(familyId, total, approved, rejected)

每轮审核结束后调用。

```javascript
onReviewRoundEnd(familyId, total, approved, rejected) {
    // familyId: 家族 ID
    // total:    本轮检查总数
    // approved: 本轮通过数
    // rejected: 本轮拒绝数
}
```

### 5.8 onError(familyId, type, errorMessage)

发生错误时调用。

```javascript
onError(familyId, type, errorMessage) {
    // familyId:      家族 ID
    // type:          错误来源类型
    // errorMessage:  错误描述
}
```

### 5.9 onPause(familyId)

家族审核暂停时调用。

```javascript
onPause(familyId) {
    // familyId: 家族 ID
}
```

### 5.10 onResume(familyId)

家族审核恢复时调用。

```javascript
onResume(familyId) {
    // familyId: 家族 ID
}
```

---

## 6. 完整 API 参考

以下所有函数在插件 JS 代码中作为全局函数可用。

### 6.1 日志与输出

#### log(level, message)

向日志系统输出消息。

```javascript
log("info", "这是一条信息");
log("warn", "这是一条警告");
log("error", "这是一条错误");
log("debug", "这是一条调试信息");

// 参数:
//   level:   "info" | "warn" | "error" | "debug" | "err"
//   message: 字符串消息
// 返回值: 无
```

#### print(...args)

向 stdout 输出（调试用）。

```javascript
print("变量值:", someValue, "结束");
// 参数: 任意数量参数，自动转字符串
// 返回值: 无
```

### 6.2 配置查询

#### getConfig()

获取全局配置。

```javascript
let cfg = getConfig();
// 返回值: 对象
//   cfg.sign_const: string
//   cfg.base_url: string
//   cfg.check_interval_seconds: number
//   cfg.concurrency: number
//   cfg.request_delay_ms: number
//   cfg.enable_regex: boolean
//   cfg.max_up_resource_coin: number
//   cfg.tui_enabled: boolean
//   cfg.web: { enabled, port, bind, root }
```

#### getTokenConfig(tokenIndex)

获取指定 Token 的配置。

```javascript
let tok = getTokenConfig(0);
// 参数: tokenIndex - 从 0 开始的索引
// 返回值: 对象或 null
//   tok.token: string (已脱敏)
//   tok.uid: string
//   tok.enable_regex: boolean
//   tok.enable_bad_words: boolean
//   tok.concurrency: number
//   tok.request_delay_ms: number
```

#### getFamilyConfig(tokenIndex, familyIndex)

获取指定家族配置。

```javascript
let fam = getFamilyConfig(0, 0);
// 参数: tokenIndex, familyIndex - 从 0 开始的索引
// 返回值: 对象或 null
//   fam.family_id: string
//   fam.mid: string
//   fam.enable_post: boolean
//   fam.enable_comment: boolean
//   fam.enable_join: boolean
//   fam.enable_up: boolean
//   fam.enable_up_resource: boolean
//   fam.min_level: number
```

### 6.3 运行状态

#### getRunningStatus()

获取运行状态信息。

```javascript
let status = getRunningStatus();
// 返回值: 对象
//   status.family_count: number    - 家族数量
//   status.token_count: number     - Token 数量
//   status.uptime_seconds: number  - 运行时长（秒）
//   status.is_paused: boolean      - 是否暂停
```

#### getFamilyCount()

获取家族总数。

```javascript
let count = getFamilyCount();  // 返回 number
```

#### getTokenCount()

获取 Token 总数。

```javascript
let count = getTokenCount();  // 返回 number
```

#### isPaused(familyId)

检查指定家族是否暂停。

```javascript
let paused = isPaused(familyId);
// 参数: familyId - 家族 ID
// 返回值: boolean
```

### 6.4 统计信息

#### getStats(familyId)

获取指定家族的审核统计。

```javascript
let stats = getStats(familyId);
// 参数: familyId - 家族 ID（可选，省略则返回全部汇总）
// 返回值: 对象或 null
//   stats.post:       { total, approved, rejected, rate }
//   stats.comment:    { total, approved, rejected, rate }
//   stats.join:       { total, approved, rejected, rate }
//   stats.up:         { total, approved, rejected, rate }
//   stats.up_resource:{ total, approved, rejected, rate }
//
// 各模块的 rate 为通过率百分比（0-100），total 为 0 时 rate 为 0
```

#### getFamilyStats(familyId)

获取家族统计的别名，同 getStats。

### 6.5 违禁词

#### getBadWords()

获取当前加载的违禁词列表。

```javascript
let words = getBadWords();
// 返回值: 字符串数组
```

#### getBadWordsEnabled()

检查违禁词检查是否启用。

```javascript
let enabled = getBadWordsEnabled();
// 返回值: boolean
```

#### getRegexPatternsCount()

获取正则违禁词数量。

```javascript
let count = getRegexPatternsCount();
// 返回值: number
```

#### getRegexEnabled()

检查正则违禁词检查是否启用。

```javascript
let enabled = getRegexEnabled();
// 返回值: boolean
```

### 6.6 HTTP 请求

#### httpGet(url, options?)

发送 HTTP GET 请求。

```javascript
let resp = httpGet("https://api.example.com/data");
// 或带选项:
let resp = httpGet("https://api.example.com/data", {
    headers: { "Authorization": "Bearer xxx" },
    timeout: 15  // 超时秒数（默认 30）
});

// 返回值: 对象
//   resp.status: number       - HTTP 状态码
//   resp.body: string         - 响应体
//   resp.ok: boolean          - status 在 200-299 之间
//   resp.error: string|undefined - 错误信息（失败时）
```

#### httpPost(url, data, options?)

发送 HTTP POST 请求。

```javascript
let resp = httpPost("https://api.example.com/data", { key: "value" });
// data 可以是对象（自动 JSON 编码）或字符串
// 返回值同 httpGet

let resp = httpPost("https://api.example.com/data", "raw body", {
    headers: { "Content-Type": "text/plain" },
    timeout: 10
});
```

### 6.7 文件系统

> 文件 I/O 限制在 `plugins/data/<plugin_name>/` 目录内，无法访问外部路径。

#### readFile(path)

读取文件内容。

```javascript
let content = readFile("config.json");
// 参数: 相对路径（相对于插件数据目录）
// 返回值: string | null（失败时返回 null）
```

#### writeFile(path, content)

写入文件内容。

```javascript
let ok = writeFile("output.txt", "Hello World");
// 参数: path - 相对路径, content - 字符串内容
// 返回值: boolean
```

#### readDir(path)

列出目录内容。

```javascript
let files = readDir(".");
// 返回值: 字符串数组 | null
```

#### fileExists(path)

检查文件是否存在。

```javascript
let exists = fileExists("config.json");
// 返回值: boolean
```

### 6.8 KV 存储

插件专用键值存储，自动持久化到 `plugins/data/<plugin_name>/store.json`。

#### kvGet(key)

读取 KV 值。

```javascript
let val = kvGet("myKey");
// 返回值: 任意类型（存储时的原始类型），key 不存在时返回 null
```

#### kvSet(key, value)

写入 KV 值。

```javascript
kvSet("myKey", { nested: { value: 42 } });
// value 可以是 string, number, boolean, object, array, null
// 返回值: 无
```

#### kvDel(key)

删除 KV 键。

```javascript
kvDel("myKey");
// 返回值: 无
```

#### kvKeys()

获取所有 KV 键名。

```javascript
let keys = kvKeys();
// 返回值: 字符串数组
```

#### kvClear()

清空所有 KV 数据。

```javascript
kvClear();
// 返回值: 无
```

### 6.9 时间

#### timestamp()

获取当前 Unix 时间戳。

```javascript
let now = timestamp();
// 返回值: number（秒级时间戳）
```

#### sleep(seconds)

异步等待指定秒数。**不能在前检查/后检查 hook 中使用**。

```javascript
await sleep(1.5);  // 等待 1.5 秒
// 参数: seconds - number，可以是小数
// 返回值: 无
```

### 6.10 编码

#### md5(str)

计算 MD5 哈希。

```javascript
let hash = md5("hello");
// 返回值: string（32 位小写十六进制）
```

#### sha1(str)

计算 SHA-1 哈希。

```javascript
let hash = sha1("hello");
// 返回值: string（40 位小写十六进制）
```

#### sha256(str)

计算 SHA-256 哈希。

```javascript
let hash = sha256("hello");
// 返回值: string（64 位小写十六进制）
```

#### base64Encode(str)

Base64 编码。

```javascript
let encoded = base64Encode("hello");
// 返回值: string
```

#### base64Decode(str)

Base64 解码。

```javascript
let decoded = base64Decode("aGVsbG8=");
// 返回值: string | null（失败时返回 null）
```

#### hexEncode(str)

十六进制编码。

```javascript
let encoded = hexEncode("hello");
// 返回值: string
```

#### hexDecode(str)

十六进制解码。

```javascript
let decoded = hexDecode("68656c6c6f");
// 返回值: string | null（失败时返回 null）
```

#### urlEncode(str)

URL 编码。

```javascript
let encoded = urlEncode("hello world");
// 返回值: string
```

#### urlDecode(str)

URL 解码。

```javascript
let decoded = urlDecode("hello+world");
// 返回值: string | null（失败时返回 null）
```

### 6.11 JSON

#### jsonParse(str)

解析 JSON 字符串。

```javascript
let obj = jsonParse('{"key": "value"}');
// 返回值: 解析后的对象 | null
```

#### jsonStringify(obj, pretty?)

序列化对象为 JSON 字符串。

```javascript
let str = jsonStringify({ a: 1, b: 2 });
let pretty = jsonStringify({ a: 1 }, true);  // 格式化输出
// 返回值: string | null
```

### 6.12 系统命令（受限）

#### exec(command)

执行系统命令。默认禁用，需在插件配置中设置 `allow_exec = true`。

```javascript
let result = exec("ls -la");
// 返回值: 对象
//   result.code: number       - 退出码
//   result.stdout: string     - 标准输出
//   result.stderr: string     - 错误输出
```

---

## 7. KV 存储

KV 存储是每个插件独立且自动持久化的键值存储。

### 存储位置

`plugins/data/<plugin_name>/store.json`

### 工作原理

- 插件加载时自动从文件中读取
- 插件卸载时自动写入文件
- 每次 `kvSet` 和 `kvDel` 操作即时写入磁盘
- 数据格式为 JSON

### 典型用途

```javascript
module.exports = {
    name: "counter",
    onLoad(config) {
        // 恢复上次运行的状态
        this.count = kvGet("runCount") || 0;
        this.count++;
        kvSet("runCount", this.count);
        log("info", `这是第 ${this.count} 次运行`);
    }
};
```

---

## 8. 文件系统访问

插件可以读写 `plugins/data/<plugin_name>/` 目录内的文件。路径穿越攻击被阻止。

### 安全限制

- 只能访问 `plugins/data/<plugin_name>/` 目录
- `../` 路径被阻止
- 绝对路径被阻止
- 目录不存在时自动创建

### 典型用途

```javascript
// 保存运行时数据
writeFile("runtime.json", JSON.stringify(runtimeData));

// 读取配置文件
let cfg = readFile("config.json");
if (cfg) {
    let parsed = JSON.parse(cfg);
    // ...
}

// 检查缓存是否存在
if (fileExists("cache.json")) {
    let cache = JSON.parse(readFile("cache.json"));
}
```

---

## 9. HTTP 请求

插件可以通过 `httpGet` 和 `httpPost` 发送 HTTP 请求，用于 Webhook 通知或外部 API 调用。

### 请求头默认值

```
User-Agent: auto_review-plugin/1.0
Accept: application/json, */*
Content-Type: application/json（POST 且 data 为对象时）
```

### 错误处理

```javascript
let resp = httpGet("https://api.example.com/data");
if (resp.ok) {
    let data = JSON.parse(resp.body);
    // 处理成功响应
} else if (resp.error) {
    log("error", `请求失败: ${resp.error}`);
} else {
    log("error", `HTTP ${resp.status}: ${resp.body}`);
}
```

### 超时控制

默认超时 30 秒，可通过 `options.timeout` 自定义：

```javascript
let resp = httpGet("https://api.example.com", { timeout: 5 });  // 5 秒超时
```

---

## 10. 编码工具

### 使用场景示例

```javascript
// 计算签名
function signRequest(params, secret) {
    let sorted = Object.keys(params).sort();
    let str = sorted.map(k => `${k}=${params[k]}`).join("&");
    return md5(str + secret);
}

// 简单的数据校验
let hash = hexEncode(md5(data));
if (storedHash === hash) {
    log("info", "数据校验通过");
}

// URL 参数编码
function buildUrl(base, params) {
    let query = Object.entries(params)
        .map(([k, v]) => `${urlEncode(k)}=${urlEncode(v)}`)
        .join("&");
    return base + "?" + query;
}
```

---

## 11. 安全与沙箱

### 隔离性

- 每个插件拥有独立的 JSRuntime + JSContext
- 插件之间不能互相访问
- 插件崩溃不会影响主程序或其他插件
- 内存限制：每个 Runtime 最多 64MB

### 沙箱限制

以下全局对象在沙箱中被移除：

- `eval()` - 字符串作为代码执行
- `Function` 构造函数
- `WebAssembly` - WASM 执行
- `import()` / `require()` - 动态导入
- 全局 `this`（严格模式下可用）

### exec() 安全

`exec()` API 默认禁用，需要：

1. 在插件配置中设置 `allow_exec = true`
2. 仅对完全信任的插件开启

### 文件系统安全

- 文件 I/O 限制在 `plugins/data/<plugin_name>/` 目录
- 路径穿越检查：拒绝包含 `..` 或绝对路径的请求

---

## 12. 调试与日志

### 日志级别

```javascript
log("debug", "详细信息");   // 仅开发调试用
log("info",  "一般信息");    // 常规运行信息
log("warn",  "警告");       // 需要注意但不影响运行
log("error", "错误");       // 需要关注的错误
```

日志会同时出现在：

- TUI 界面的日志面板
- stdout（TUI 未启动时）

### 使用 print

```javascript
print("调试输出:", JSON.stringify(data, null, 2));
// print 总是输出到 stdout
```

### 错误处理建议

```javascript
try {
    let resp = httpGet("https://api.example.com");
    if (!resp.ok) {
        log("error", `API 返回错误: ${resp.status}`);
    }
} catch (e) {
    log("error", `请求异常: ${e.toString()}`);
}
```

---

## 13. 最佳实践

### 插件设计原则

1. **保持同步 hook 轻量**：`onBeforeCheck` 和 `onAfterCheck` 在审核热点路径中调用，应避免耗时操作
2. **使用 KV 存储而不是文件**：KV 存储自动管理持久化，比手动文件操作更安全
3. **错误处理**：始终用 try/catch 包裹可能失败的调用
4. **日志分级**：使用恰当的日志级别，避免生产环境输出过多 debug 日志
5. **配置参数化**：通过 `settings.toml` 的 `[PLUGINS.LOAD.config]` 暴露可调参数
6. **避免全局状态**：使用 `this` 属性存储插件状态
7. **幂等设计**：hook 函数应设计为可重复调用无副作用

### 性能注意事项

- `onBeforeCheck` / `onAfterCheck` 每项文本检查都会调用，避免在其中发送 HTTP 请求
- 耗时操作（HTTP 请求、文件写入）放在事件 hook 中
- 善用 `this` 缓存计算结果

### 配置示例

```toml
[[PLUGINS.LOAD]]
name = "my_filter"
file = "my_filter.js"
enabled = true
[PLUGINS.LOAD.config]
threshold = 0.8
endpoint = "https://my-service.example.com/check"
timeout = 5000
```

---

## 14. 完整示例

完整示例插件位于 `plugins/examples/complete.js`，该示例演示了所有 10 个 hooks 和主要 API 的用法。包括：

- 插件加载与配置读取
- KV 存储状态持久化
- 所有 hook 回调的实现
- HTTP Webhook 通知
- 文件读写
- 运行报告生成

启用方式：取消 `settings.toml` 中对应 `[[PLUGINS.LOAD]]` 的注释并将 `enabled` 设为 `true`。
