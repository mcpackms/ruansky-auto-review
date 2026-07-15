// node_host.js - Node.js 插件宿主
//
// 通过 stdin/stdout JSON 行协议与 C++ 主进程通信。
// 每个插件启动一个独立的 node 进程实例。
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
            if (full.indexOf(dataDir) !== 0) return null;
            return fs.readFileSync(full, "utf-8");
        } catch (e) {
            return null;
        }
    },
    writeFile(relPath, content) {
        try {
            const full = path.join(dataDir, relPath);
            if (full.indexOf(dataDir) !== 0) return false;
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
            if (full.indexOf(dataDir) !== 0) return false;
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

// 加载插件
async function loadPlugin(filePath, config) {
    try {
        pluginConfig = config;

        // 将 API 注入为全局变量（兼容旧插件用法）
        for (const [key, fn] of Object.entries(api)) {
            global[key] = fn;
        }
        // 额外别名
        global.httpGet = httpGet;
        global.httpPost = httpPost;

        try { delete require.cache[require.resolve(filePath)]; } catch(e) {}
        plugin = require(filePath);

        if (typeof plugin !== "object" || plugin === null) {
            sendResponse(0, { ok: false, error: "module.exports 不是对象" });
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
                    await ret;
                }
            } catch (e) {
                console.error("[插件] onLoad 错误:", e.message);
            }
        }

        sendResponse(0, { ok: true });
    } catch (e) {
        sendResponse(0, { ok: false, error: e.message });
    }
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
