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
10. [调试与日志](#10-调试与日志)
11. [最佳实践](#11-最佳实践)
12. [完整示例](#12-完整示例)
13. [安全注意事项](#13-安全注意事项)
14. [通信协议](#14-通信协议)

---

## 1. 概述

auto_review 支持使用 JavaScript 编写审核插件，通过 **Node.js 子进程 + `vm.createContext` VM 沙箱**执行。
每个插件启动一个独立的 `node` 进程，进程内通过 `vm.createContext` 创建隔离的沙箱上下文运行插件代码，
通过 stdin/stdout JSON 行协议与 C++ 主程序通信。

> 🔒 **安全设计**：沙箱封锁了 `child_process`、`net`、`vm`、`fs` 等高危模块，
> 插件只能通过注入的安全 API（HTTP、KV 存储、受限文件 I/O）和内置白名单模块（`crypto`、`path`、`url` 等）进行开发。
> 详情见本章[安全注意事项](#14-安全注意事项)。

特性：

- **沙箱执行**：插件代码在 `vm.createContext` VM 上下文中运行，封锁高危模块
- 每个插件运行在独立 OS 进程中，进程级隔离，崩溃不影响主程序
- 支持使用 `npm` 安装的第三方库（仅限不依赖被封锁模块的安全包）
- 支持 `async/await`（Node.js 原生）
- 自动扫描目录加载 `.js` 文件，无需手动注册
- 10 种 hook 回调
- KV 键值存储自动持久化到 `plugins/data/<插件名>/store.json`
- 文件 I/O 限制在插件私有数据目录内，自动阻止路径穿越

---

## 2. 快速开始

### 目录结构

```
plugins/
  data/              # 插件数据目录（自动创建）
    <plugin_name>/   # 每个插件一个子目录
  examples/
    complete.js      # 完整示例
  node_host.js       # Node.js 宿主（框架文件）
  my_plugin.js       # 你的插件
```

### 最简单的插件

创建 `plugins/my_plugin.js`：

```javascript
module.exports = {
    name: "my_plugin",
    version: "1.0.0",
    description: "我的第一个插件",

    onLoad(config) {
        log("info", "插件已加载");
    },

    onReviewRoundEnd(familyId, total, approved, rejected) {
        log("info", "家族 " + familyId + " 本轮: " + total + " 项");
    }
};
```

将文件放入 `plugins/` 目录，下次启动时自动加载。

### 前置条件

系统需要安装 **Node.js >= 18**（`vm.createContext` 沙箱需要 Node.js 内置 `vm` 模块）：

```bash
# Debian/Ubuntu
sudo apt install nodejs

# Termux
pkg install nodejs

# Arch Linux
sudo pacman -S nodejs
```

---

## 3. 插件配置

### settings.toml 格式

```toml
[PLUGINS]
# 插件搜索目录列表，扫描其中所有 .js 文件并自动加载
dirs = ["./plugins", "./plugins/examples"]

# 手动配置单个插件（可选，覆盖自动加载的同名插件）
[[PLUGINS.LOAD]]
name = "my_plugin"          # 插件名称（必填）
file = "my_plugin.js"       # JS 文件路径，相对于 dirs
enabled = true              # 启用

# 插件自定义配置，JS 中通过 onLoad(config) 接收
[PLUGINS.LOAD.config]
key1 = "value1"
key2 = 42
```

### 加载顺序

1. **Phase 1**: 处理 `[[PLUGINS.LOAD]]` 手动配置项
2. **Phase 2**: 扫描所有 `dirs` 目录，自动加载未加载的 `.js` 文件

同名插件以手动配置为准（自动扫描跳过已加载的名称）。

---

## 4. 插件生命周期

```
启动时:
  1. 解析 settings.toml，读取 [PLUGINS] 配置
  2. 对每个插件:
     a. fork 子进程，exec node 运行 node_host.js
     b. node 进程初始化，写入就绪消息到 stdout
     c. C++ 父进程读取就绪消息，发送 load 命令
     d. node 进程加载插件 JS 文件，调用 onLoad(config)
     e. 加载完成

运行时:
  - 同步 hook（onBeforeCheck, onAfterCheck）：C++ 发送命令，等待 JSON 响应
  - 异步 hook（其余事件）：C++ 发送命令，不等待立即返回

停止时:
  1. C++ 向各子进程发送 shutdown 命令
  2. 等待子进程退出（最长 1 秒）
  3. 超时未退出则 SIGKILL
```

---

## 5. 完整 Hook 参考

所有 hook 在 `module.exports` 对象上定义，可选实现。

### 5.1 onLoad(config)

插件加载时调用一次。

```javascript
onLoad(config) {
    // config: 插件配置对象（来自 settings.toml [PLUGINS.LOAD.config]）
    // this: 插件实例（可在 this 上存储状态）
}
```

### 5.2 onBeforeCheck(text, familyId, type)

文本检查前调用。**阻塞等待 Node.js 返回结果**，应避免耗时操作。

```javascript
onBeforeCheck(text, familyId, type) {
    // text:     待检查的原始文本
    // familyId: 家族 ID
    // type:     "post" | "comment" | "up" | "up_resource"
    //
    // 返回值:
    //   null / undefined       -> 继续默认检查
    //   { reject: true, reason: "..." }  -> 直接拒绝
    //   { reject: false }                -> 直接通过
}
```

### 5.3 onAfterCheck(text, result, familyId, type)

文本检查后调用。**阻塞等待**。

```javascript
onAfterCheck(text, result, familyId, type) {
    // result: { reject: bool, reason: string }  默认检查结果
    // 返回值同 onBeforeCheck
}
```

### 5.4 onItemApproved(familyId, type, itemId)

项目通过时调用（异步）。

### 5.5 onItemRejected(familyId, type, itemId, reason)

项目被拒绝时调用（异步）。

### 5.6 onReviewRoundStart(familyId)

每轮审核开始前调用（异步）。

### 5.7 onReviewRoundEnd(familyId, total, approved, rejected)

每轮审核结束后调用（异步）。适合在此输出统计报告。

### 5.8 onError(familyId, type, errorMessage)

发生错误时调用（异步）。

### 5.9 onPause(familyId)

家族审核暂停时调用（异步）。

### 5.10 onResume(familyId)

家族审核恢复时调用（异步）。

---

## 6. 完整 API 参考

以下函数可在插件 JS 代码中直接调用。

### 6.1 日志

#### log(level, message)

向主程序日志系统输出消息，显示在 TUI 日志面板和 stdout。

```javascript
log("info", "普通信息");
log("warn", "警告信息");
log("error", "错误信息");
log("debug", "调试信息");
log("err", "错误简写");
```

#### print(...args)

向 stderr 输出调试信息（不会显示在 TUI 日志面板，仅终端可见）。

```javascript
print("变量值:", x);
```

### 6.2 配置

#### getConfig()

获取插件配置文件中的自定义配置（即 `[PLUGINS.LOAD.config]` 的内容）。注意：这与旧版 QuickJS 的 `getConfig()` 行为不同，旧版返回全局配置，当前版本返回插件私有配置。

```javascript
let cfg = getConfig();
// 返回值: settings.toml 中 [PLUGINS.LOAD.config] 的对象
```

### 6.3 KV 存储

每个插件独立，自动持久化到 `plugins/data/<plugin_name>/store.json`。

#### kvGet(key)

```javascript
let val = kvGet("myKey");     // 不存在返回 null
```

#### kvSet(key, value)

```javascript
kvSet("myKey", { a: 1 });     // value 支持任意 JSON 类型
```

#### kvDel(key)

```javascript
kvDel("myKey");
```

#### kvKeys()

```javascript
let allKeys = kvKeys();       // 返回字符串数组
```

#### kvClear()

```javascript
kvClear();                    // 清空所有数据
```

### 6.4 文件系统

文件 I/O 限制在 `plugins/data/<plugin_name>/` 目录内。

#### readFile(path)

```javascript
let content = readFile("data.json");   // 返回 string | null
```

#### writeFile(path, content)

```javascript
let ok = writeFile("output.txt", "内容");  // 返回 boolean
```

#### fileExists(path)

```javascript
let exists = fileExists("config.json");   // 返回 boolean
```

### 6.5 HTTP 请求

使用 Node.js 内置 `http`/`https` 模块，无需第三方库。

#### httpGet(url, options?)

```javascript
let resp = httpGet("https://api.example.com/data");
// 或带选项:
let resp = httpGet("https://api.example.com/data", { timeout: 15 });

// 返回值:
//   resp.status: number        - HTTP 状态码
//   resp.body: string          - 响应体
//   resp.ok: boolean           - status 200-299
//   resp.error: string|undefined
```

#### httpPost(url, data, options?)

```javascript
let resp = httpPost("https://api.example.com/data", { key: "value" });
// data 为对象时自动 JSON 编码并设置 Content-Type: application/json
// data 为字符串时原样发送

let resp = httpPost("https://api.example.com/data", "raw body", {
    headers: { "Content-Type": "text/plain" },
    timeout: 10
});
```

### 6.6 时间

#### timestamp()

```javascript
let now = timestamp();   // 秒级 Unix 时间戳
```

#### sleep(seconds)

返回 Promise，可在 async hook 中 await。

```javascript
await sleep(1.5);        // 等待 1.5 秒
```

注意：不能在 `onBeforeCheck` / `onAfterCheck` 中使用（会阻塞主进程）。

---

## 7. KV 存储

KV 存储是每个插件独立、自动持久化的键值存储。

### 存储位置

`plugins/data/<plugin_name>/store.json`

### 工作原理

- 插件加载时自动从文件读取
- `kvSet` 和 `kvDel` 即时写入磁盘
- `kvClear` 清空并写入
- 进程退出时自动保存

### 典型用途

```javascript
module.exports = {
    name: "counter",
    onLoad(config) {
        let count = kvGet("runCount") || 0;
        count++;
        kvSet("runCount", count);
        log("info", "第 " + count + " 次运行");
    }
};
```

---

## 8. 文件系统访问

### 安全限制

- 只能读写 `plugins/data/<plugin_name>/` 目录
- `../` 路径穿越被阻止
- 绝对路径被阻止
- 目录不存在时自动创建

### 典型用途

```javascript
// 写入运行数据
writeFile("runtime.json", JSON.stringify(data));

// 读取配置文件
let raw = readFile("config.json");
if (raw) {
    let cfg = JSON.parse(raw);
}
```

---

## 9. HTTP 请求

### 错误处理

```javascript
let resp = httpGet("https://api.example.com/data");
if (resp.ok) {
    let data = JSON.parse(resp.body);
} else if (resp.error) {
    log("error", "请求失败: " + resp.error);
} else {
    log("warn", "HTTP " + resp.status + ": " + resp.body);
}
```

### 超时

```javascript
let resp = httpGet("https://api.example.com", { timeout: 5 });
```

默认超时 30 秒。

---

## 10. 调试与日志

### 日志级别

```javascript
log("debug", "详细信息");    // 开发调试
log("info",  "一般信息");     // 常规运行
log("warn",  "警告");        // 需关注但不影响运行
log("error", "错误");        // 需处理的错误
```

日志出现在：

- TUI 日志面板（带 `[插件:<name>]` 前缀）
- stdout（TUI 未启动时）

### print 输出

```javascript
print("调试:", JSON.stringify(data));
// 输出到 stderr，终端可见
```

### 错误处理建议

```javascript
try {
    let resp = httpGet("https://api.example.com");
    if (!resp.ok) {
        log("error", "API 错误: " + resp.status);
    }
} catch (e) {
    log("error", "异常: " + e.toString());
}
```

---

## 11. 最佳实践

### 设计原则

1. **同步 hook 保持轻量**：`onBeforeCheck` / `onAfterCheck` 阻塞等待结果，避免在其中做 HTTP 请求或大量计算
2. **异步 hook 做耗时操作**：HTTP 请求、文件写入放在事件 hook 中
3. **善用 KV 存储**：自动持久化，比手动文件操作更安全
4. **错误处理**：用 try/catch 包裹可能失败的调用
5. **配置参数化**：通过 `settings.toml` 暴露可调参数
6. **状态放在 `module.exports` 上**：避免使用全局变量
7. **安全第一**：沙箱不提供 `child_process`、`net`、`fs` 等模块。若需要 HTTP 通信，使用注入的 `httpGet()` / `httpPost()`；若需要文件读写，使用 `readFile()` / `writeFile()` API

### 性能注意事项

- 同步 hook 需要 C++ 等待 Node.js 返回结果，有进程间通信开销
- 每个插件一个独立 node 进程，内存约 20-40MB
- 建议生产环境仅加载必要的插件

### Node.js 特有能力

插件可以通过 `require()` 使用以下**白名单安全内置模块**：

| 模块 | 用途 |
|------|------|
| `crypto` | 加密、哈希、随机数 |
| `path` | 路径处理 |
| `url` | URL 解析 |
| `assert` | 断言测试 |
| `buffer` | 二进制数据处理 |
| `events` | 事件驱动 |
| `util` | 实用工具函数 |
| `querystring` | 查询字符串解析 |
| `string_decoder` | 字符串解码 |
| `zlib` | 压缩 |
| `punycode` | Punycode 编码 |
| `text_decoder` | 文本编码/解码 |

```javascript
// 在 onLoad 或异步 hook 中使用
const crypto = require("crypto");
const path = require("path");

module.exports = {
    onLoad(config) {
        let hash = crypto.createHash("sha256").update("data").digest("hex");
        log("info", "SHA256: " + hash);
    }
};
```

### 已封锁的高危模块

以下模块在插件沙箱中**被封锁**，无法通过 `require()` 加载：

| 模块 | 原因 |
|------|------|
| `child_process` | 命令执行 |
| `cluster` | 多进程控制 |
| `net` | 网络连接 |
| `tls` | 安全网络连接 |
| `tty` | 终端控制 |
| `dgram` | UDP 通信 |
| `dns` | DNS 查询 |
| `vm` | 沙箱逃逸 |
| `worker_threads` | 线程执行 |
| `module` | 模块系统操纵 |
| `v8` | V8 引擎访问 |
| `natives` / `native_module` | V8 内部 API |
| `process` | 进程控制 |

> **注意**：`fs` 模块在沙箱中被移除。请使用内置的 `readFile()`、`writeFile()`、`fileExists()` API，
> 这些 API 自动将路径限制在 `plugins/data/<插件名>/` 目录下，并阻止路径穿越攻击。
> 同时 `http` 和 `https` 模块不在白名单内，应使用注入的 `httpGet()` / `httpPost()` API 发起 HTTP 请求。

### npm 第三方包

沙箱允许加载 `node_modules` 目录中的 npm 包，但同样受模块白名单限制。
如果第三方包依赖被封锁的模块（如 `child_process`），该包将被阻止加载并抛出安全异常。

```javascript
// 允许：安全的 npm 包
const lodash = require("lodash");

// 阻止：依赖 child_process 的包
// require("some-malicious-package");  // 抛出 [安全] 模块错误
```

---

## 12. 完整示例

完整示例见 `plugins/examples/complete.js`，演示了所有 10 种 hook 和主要 API 的用法：

- 插件加载与配置读取
- KV 存储状态持久化
- 所有 hook 回调
- HTTP Webhook 通知
- 文件读写
- 运行报告生成

启用：确认 `settings.toml` 的 `dirs` 包含 `./plugins/examples`。

---

## 13. 安全注意事项

### 沙箱执行

插件代码在 `vm.createContext` 创建的独立 V8 上下文中执行。该沙箱：

- 不提供 `require` 对 `child_process`、`net`、`fs`、`vm`、`worker_threads` 等高危模块的访问
- 不提供 `globalThis.constructor.constructor('return this')()` 等沙箱逃逸路径（代码以 `vm.Script.runInContext` 运行，`Function` 构造函数被沙箱替代）
- 设置 5 秒执行超时，防止死循环
- `eval()` 在沙箱上下文中受限于该上下文范围

### 常见安全误区

| ❌ 错误做法 | ✅ 正确做法 |
|-----------|-----------|
| `require("child_process").execSync("rm -rf /")` | 被封锁，抛出安全异常 |
| `require("fs").writeFileSync("/etc/passwd", "...")` | 被封锁，使用 `writeFile()` 代替 |
| `require("http").get(...)` | 被封锁，使用 `httpGet()` 代替 |
| `new Function('return process')()` | 在沙箱中 `Function` 受限，无法逃逸 |
| 直接在插件代码中拼接用户输入到 `require()` | 可能泄露路径信息，应使用固定模块名 |

### 文件 I/O

内置的 `readFile()`、`writeFile()`、`fileExists()` 会：

1. 将相对路径拼接到 `plugins/data/<插件名>/` 目录下
2. 使用 `path.relative()` + 检查 `..` 前缀来阻止路径穿越
3. 自动创建数据目录（如果不存在）

### HTTP 请求

注入的 `httpGet()` / `httpPost()`：

- 使用 Node.js 内置 `http`/`https` 模块（在宿主编程中，不在沙箱中暴露这两个模块）
- 默认 30 秒超时，可通过 `options.timeout` 调整
- 不暴露底层 `http.Agent` 或 `net.Socket` 等危险对象

---

## 14. 通信协议

C++ 主程序与 Node.js 子进程通过 stdin/stdout JSON 行协议通信。

### 数据格式

每条消息为一行 JSON，以 `\n` 结尾。

### 命令（C++ -> Node）

```
{"id":0, "cmd":"load", "file":"/abs/path/plugin.js", "config":{...}}
{"id":1, "cmd":"call", "hook":"onBeforeCheck", "args":["text", "fid", "type"]}
{"id":0, "cmd":"call", "hook":"onItemApproved", "args":["fid", "type", "id"]}
{"id":-1,"cmd":"shutdown"}
```

### 响应（Node -> C++）

```
{"ready":true, "name":"plugin_name"}                    // 就绪消息（启动后立即发送）
{"id":0, "ok":true}                                      // load 成功
{"id":1, "ok":true, "result":{...}}                      // sync hook 返回结果
{"ok":false, "error":"..."}                              // 错误
{"id":-1,"ok":true}                                      // shutdown 确认
```

### 同步 vs 异步

- **同步命令**（`load`, `onBeforeCheck`, `onAfterCheck`）：C++ 发送后阻塞读取响应
- **异步命令**（其余 hook）：C++ 发送后立即返回，不等待响应
- 异步命令的 `id` 固定为 0，C++ 不处理其响应

### 日志输出

Node.js 插件的 `log()` 和 `print()` 通过 `console.error()` 写入 stderr，C++ 不读取 stderr，直接输出到终端。
