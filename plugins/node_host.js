// node_host.js - Node.js 插件宿主
// Copyright (C) 2026 YIZHIDIANBI (一支电笔)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.
//
// 通过 stdin/stdout JSON 行协议与 C++ 主进程通信。
// 每个插件启动一个独立的 node 进程实例。
//
// 安全：插件运行在 vm.createContext 沙箱中，高危模块被封锁。
//
// 通信通道：
//   stdin  - C++ 写入命令（JSON Lines）
//   stdout - Node 写入响应（JSON Lines），仅同步命令有响应
//   stderr - Node 写入日志/错误输出，C++ 不读取
//
// 协议格式：
//   C++ -> Node: {"id":1,"cmd":"load","file":"...","config":{}}
//   Node -> C++: {"id":1,"ok":true}
//
//   C++ -> Node: {"id":2,"cmd":"call","hook":"onBeforeCheck","args":["text","fid","type"]}
//   Node -> C++: {"id":2,"ok":true,"result":{"shouldReject":false}}
//
//   C++ -> Node: {"id":3,"cmd":"call","hook":"onItemApproved","args":["fid","type","id"]}
//   (不等待响应，Node 可自行处理)
//
//   C++ -> Node: {"id":0,"cmd":"shutdown"}
//   Node -> C++: {"id":0,"ok":true}
//   Node 退出

const fs = require("fs");
const path = require("path");
const vm = require("vm");

const pluginFile = path.resolve(process.argv[2]);
const pluginName = process.argv[3] || "unknown";
const dataDir = path.resolve(process.argv[4] || "./plugins/data/" + pluginName);

let plugin = null;
let pluginConfig = null;
let store = {};

// ---- 持久化 KV 存储 ----
const storeFile = path.join(dataDir, "store.json");

function loadStore() {
    try {
        const raw = fs.readFileSync(storeFile, "utf-8");
        store = JSON.parse(raw);
    } catch (e) {
        store = {};
    }
}

function saveStore() {
    try {
        if (!fs.existsSync(dataDir)) {
            fs.mkdirSync(dataDir, { recursive: true });
        }
        fs.writeFileSync(storeFile, JSON.stringify(store, null, 2), "utf-8");
    } catch (e) {
        console.error("[store] 写入失败:", e.message);
    }
}

// ---- 内置 API ----
const api = {
    log(level, msg) {
        const prefix = level === "error" || level === "err" ? "[ERROR]"
                     : level === "warn" || level === "warning" ? "[WARN]"
                     : level === "debug" ? "[DEBUG]"
                     : "[INFO]";
        console.error(`${prefix} [插件:${pluginName}] ${msg}`);
    },
    print(...args) {
        console.error(args.join(" "));
    },
    getConfig() {
        return pluginConfig || {};
    },
    timestamp() {
        return Math.floor(Date.now() / 1000);
    },
    sleep(secs) {
        return new Promise(r => setTimeout(r, secs * 1000));
    },
    kvGet(key) {
        return store.hasOwnProperty(key) ? store[key] : null;
    },
    kvSet(key, value) {
        store[key] = value;
        saveStore();
    },
    kvDel(key) {
        delete store[key];
        saveStore();
    },
    kvKeys() {
        return Object.keys(store);
    },
    kvClear() {
        store = {};
        saveStore();
    },
    readFile(relPath) {
        try {
            const full = path.join(dataDir, relPath);
            const rel = path.relative(dataDir, full);
            if (rel.startsWith('..') || path.isAbsolute(rel)) return null;
            return fs.readFileSync(full, "utf-8");
        } catch (e) {
            return null;
        }
    },
    writeFile(relPath, content) {
        try {
            const full = path.join(dataDir, relPath);
            const rel = path.relative(dataDir, full);
            if (rel.startsWith('..') || path.isAbsolute(rel)) return false;
            if (!fs.existsSync(dataDir)) fs.mkdirSync(dataDir, { recursive: true });
            fs.writeFileSync(full, content, "utf-8");
            return true;
        } catch (e) {
            return false;
        }
    },
    fileExists(relPath) {
        try {
            const full = path.join(dataDir, relPath);
            const rel = path.relative(dataDir, full);
            if (rel.startsWith('..') || path.isAbsolute(rel)) return false;
            return fs.existsSync(full);
        } catch (e) {
            return false;
        }
    }
};

// ---- JSON Lines 通信 ----
const readline = require("readline");
const rl = readline.createInterface({
    input: process.stdin,
    output: process.stdout,
    terminal: false
});

function sendResponse(id, data) {
    data.id = id;
    process.stdout.write(JSON.stringify(data) + "\n");
}

// ==================== VM 沙箱 ====================

// 封锁的高危模块列表
const BLOCKED_MODULES = new Set([
    "child_process", "cluster", "vm", "worker_threads",
    "net", "tls", "tty", "dgram", "dns",
    "module", "process", "v8", "native_module", "natives"
]);

// 安全的内置模块（白名单）
const SAFE_BUILTIN_MODULES = new Set([
    "assert", "buffer", "crypto", "events", "path",
    "querystring", "string_decoder", "url", "util", "zlib",
    "punycode", "text_decoder"
]);

// 受限模块的增强限制
function createSandboxRequire(pluginFilePath) {
    // 缓存已加载的模块
    const moduleCache = new Map();

    return function sandboxRequire(moduleName) {
        // 检查是否在白名单中
        if (!SAFE_BUILTIN_MODULES.has(moduleName)) {
            // 检查是否为被封锁的高危模块
            if (BLOCKED_MODULES.has(moduleName)) {
                throw new Error(
                    `[安全] 模块 '${moduleName}' 在插件沙箱中被封锁，不允许加载`
                );
            }
            // 对于非内置模块（npm 包），临时放行但做额外检查
            // 注意：require.resolve 可能被用来路径探测
            try {
                const resolved = require.resolve(moduleName, { paths: [path.dirname(pluginFilePath)] });
                // 检查是否是内置模块的别名
                if (BLOCKED_MODULES.has(moduleName.toLowerCase())) {
                    throw new Error(
                        `[安全] 模块 '${moduleName}' 在插件沙箱中被封锁`
                    );
                }
                // 不允许加载 node_modules 之外的任意路径
                if (resolved.indexOf("node_modules") === -1 &&
                    !SAFE_BUILTIN_MODULES.has(moduleName)) {
                    throw new Error(
                        `[安全] 只能加载内置模块或 node_modules 中的模块`
                    );
                }
            } catch (e) {
                if (e.message.startsWith("[安全]")) throw e;
                // 尝试作为内置模块加载
                if (SAFE_BUILTIN_MODULES.has(moduleName)) {
                    // 允许
                } else {
                    throw new Error(`[安全] 模块 '${moduleName}' 不允许加载或不存在`);
                }
            }
        }

        if (moduleCache.has(moduleName)) {
            return moduleCache.get(moduleName);
        }
        const mod = require(moduleName);
        moduleCache.set(moduleName, mod);
        return mod;
    };
}

// 创建插件沙箱上下文
function createSandboxContext(pluginFilePath, pluginName_, config) {
    const sandbox = {
        // ---- ECMAScript 标准全局 ----
        Array, ArrayBuffer, Boolean, DataView, Date, Error,
        EvalError, Float32Array, Float64Array, Function,
        Int8Array, Int16Array, Int32Array, Uint8Array, Uint8ClampedArray,
        Uint16Array, Uint32Array, BigInt64Array, BigUint64Array,
        Map, Set, WeakMap, WeakSet,
        Number, BigInt, Object, Promise, Proxy, RangeError,
        ReferenceError, RegExp, String, Symbol, SyntaxError,
        TypeError, URIError, AggregateError,
        JSON, Math, Intl, Reflect,
        // ---- Node 安全全局 ----
        Buffer, URL, URLSearchParams, TextEncoder, TextDecoder,
        setTimeout, clearTimeout, setInterval, clearInterval,
        setImmediate, clearImmediate, queueMicrotask,
        console: {
            log: (...args) => console.error("[插件:" + pluginName_ + "]", ...args),
            error: (...args) => console.error("[插件:" + pluginName_ + "]", ...args),
            warn: (...args) => console.error("[插件:" + pluginName_ + "]", ...args),
            info: (...args) => console.error("[插件:" + pluginName_ + "]", ...args),
            debug: (...args) => console.error("[插件:" + pluginName_ + "]", ...args),
        },
        // ---- 插件注入 API ----
        log: api.log,
        print: api.print,
        getConfig: api.getConfig,
        timestamp: api.timestamp,
        sleep: api.sleep,
        kvGet: api.kvGet,
        kvSet: api.kvSet,
        kvDel: api.kvDel,
        kvKeys: api.kvKeys,
        kvClear: api.kvClear,
        readFile: api.readFile,
        writeFile: api.writeFile,
        fileExists: api.fileExists,
        httpGet: httpGet,
        httpPost: httpPost,
        // ---- 沙箱 require ----
        require: createSandboxRequire(pluginFilePath),
        // ---- CommonJS 模块系统 ----
        module: { exports: {} },
        exports: {},
        __dirname: path.dirname(pluginFilePath),
        __filename: pluginFilePath,
        // ---- 沙箱标记（用于检测逃逸） ----
        __sandboxed__: true,
    };

    return vm.createContext(sandbox);
}

// 在沙箱中加载插件
async function loadPluginInSandbox(filePath, config) {
    pluginConfig = config;

    const context = createSandboxContext(filePath, pluginName, config);
    const code = fs.readFileSync(filePath, "utf-8");

    try {
        const script = new vm.Script(code, {
            filename: filePath,
            lineOffset: 0,
            displayErrors: true,
        });

        script.runInContext(context, {
            timeout: 5000,  // 5s 超时防止死循环
            breakOnSigint: true,
            displayErrors: true,
        });

        plugin = context.module.exports;

        if (typeof plugin !== "object" || plugin === null) {
            sendResponse(0, { ok: false, error: "module.exports 不是对象" });
            plugin = null;
            return;
        }

        // 注入 API 到插件实例
        plugin._api = api;
        plugin.__dirname = path.dirname(filePath);

        // 调用 onLoad（支持 async）
        if (typeof plugin.onLoad === "function") {
            try {
                const ret = plugin.onLoad(config);
                if (ret && typeof ret.then === "function") {
                    // 异步 onLoad 需要等待但设置超时
                    await Promise.race([
                        ret,
                        new Promise((_, reject) =>
                            setTimeout(() => reject(new Error("onLoad 超时")), 10000)
                        )
                    ]);
                }
            } catch (e) {
                console.error("[插件] onLoad 错误:", e.message);
            }
        }

        sendResponse(0, { ok: true });
    } catch (e) {
        sendResponse(0, { ok: false, error: e.message });
        plugin = null;
    }
}

// 加载插件（兼容旧版非沙箱路径，但走沙箱）
async function loadPlugin(filePath, config) {
    await loadPluginInSandbox(filePath, config);
}

// 调用 hook
function callHook(hookName, args, id) {
    try {
        if (!plugin || typeof plugin[hookName] !== "function") {
            sendResponse(id, { ok: true, result: null });
            return;
        }

        const result = plugin[hookName].apply(plugin, args);

        if (result && typeof result.then === "function") {
            result.then(r => {
                sendResponse(id, { ok: true, result: r !== undefined ? r : null });
            }).catch(e => {
                sendResponse(id, { ok: false, error: e.message });
            });
        } else {
            sendResponse(id, {
                ok: true,
                result: result !== undefined ? result : null
            });
        }
    } catch (e) {
        sendResponse(id, { ok: false, error: e.message });
    }
}

// ---- 主循环 ----
rl.on("line", (line) => {
    let msg;
    try {
        msg = JSON.parse(line);
    } catch (e) {
        console.error("[协议] JSON 解析失败:", line);
        return;
    }

    const id = msg.id;
    const cmd = msg.cmd;

    switch (cmd) {
        case "load":
            loadStore();
            // Resolve relative paths to absolute
            const filePath = path.resolve(msg.file);
            loadPlugin(filePath, msg.config || {}).catch(e => {
                console.error("[插件] loadPlugin 错误:", e.message);
            });
            break;

        case "call":
            callHook(msg.hook, msg.args || [], id);
            break;

        case "shutdown":
            sendResponse(id, { ok: true });
            saveStore();
            process.exit(0);
            break;

        default:
            console.error("[协议] 未知命令:", cmd);
    }
});

// ---- HTTP 请求（简单实现，不依赖第三方库） ----
function httpGet(url, options) {
    return new Promise((resolve) => {
        try {
            const parsed = new URL(url);
            const mod = parsed.protocol === "https:" ? require("https") : require("http");
            const req = mod.get(url, { timeout: (options && options.timeout) || 30000 }, (res) => {
                let body = "";
                res.on("data", chunk => body += chunk);
                res.on("end", () => {
                    resolve({ status: res.statusCode, body, ok: res.statusCode >= 200 && res.statusCode < 300 });
                });
            });
            req.on("error", (e) => resolve({ status: 0, body: "", ok: false, error: e.message }));
            req.on("timeout", () => { req.destroy(); resolve({ status: 0, body: "", ok: false, error: "timeout" }); });
        } catch (e) {
            resolve({ status: 0, body: "", ok: false, error: e.message });
        }
    });
}

function httpPost(url, data, options) {
    try {
        const parsed = new URL(url);
        const http = parsed.protocol === "https:" ? require("https") : require("http");
        const bodyStr = typeof data === "object" ? JSON.stringify(data) : String(data);
        const headers = (options && options.headers) || {};
        if (typeof data === "object" && !headers["Content-Type"]) {
            headers["Content-Type"] = "application/json";
        }
        return new Promise((resolve) => {
            const req = http.request(url, {
                method: "POST",
                headers,
                timeout: (options && options.timeout) || 30000
            }, (res) => {
                let body = "";
                res.on("data", chunk => body += chunk);
                res.on("end", () => {
                    resolve({ status: res.statusCode, body, ok: res.statusCode >= 200 && res.statusCode < 300 });
                });
            });
            req.on("error", (e) => resolve({ status: 0, body: "", ok: false, error: e.message }));
            req.on("timeout", () => { req.destroy(); resolve({ status: 0, body: "", ok: false, error: "timeout" }); });
            req.write(bodyStr);
            req.end();
        });
    } catch (e) {
        return { status: 0, body: "", ok: false, error: e.message };
    }
}

// 就绪通知（写入 stdout，父进程等待此消息后发送 load 命令）
console.error(`[插件] ${pluginName} 启动 (Node ${process.version})`);
process.stdout.write(JSON.stringify({ ready: true, name: pluginName }) + "\n");
