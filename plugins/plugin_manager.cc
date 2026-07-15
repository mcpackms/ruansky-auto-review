// plugin_manager.cc - Plugin manager implementation
#include "plugin_manager.h"
#include "js_bridge.h"
#include "js_sandbox.h"
#include "quickjs_helpers.h"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <set>
#include <sstream>
#include <sys/stat.h>

#include "config_types.h"
#include "tui.h"

// ==================== Global plugin manager ====================
PluginManager g_plugin_mgr;

// ==================== Forward declarations of extern symbols ====================
extern std::unordered_map<std::string, std::shared_ptr<FamilyControl>> g_family_controls;

// Helper to check if a file exists
static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// ==================== PluginManager ====================

PluginManager::PluginManager() {
    // Ensure data directory exists
    system("mkdir -p ./plugins/data");
}

PluginManager::~PluginManager() {
    unload_all();
}

bool PluginManager::load_from_config(const toml::value& plugins_config) {
    // Parse [PLUGINS] section
    std::vector<std::string> dirs = {"./plugins"};

    if (plugins_config.contains("dirs")) {
        dirs.clear();
        auto& dirs_arr = plugins_config.at("dirs").as_array();
        for (auto& d : dirs_arr) {
            dirs.push_back(d.as_string());
        }
    }

    // Ensure data directory exists
    system("mkdir -p ./plugins/data");

    int loaded = 0;
    int failed = 0;
    std::set<std::string> loaded_names;

    // === Phase 1: Load explicitly configured plugins ([[PLUGINS.LOAD]]) ===
    if (plugins_config.contains("LOAD")) {
        auto& loads = plugins_config.at("LOAD").as_array();
        for (size_t i = 0; i < loads.size(); ++i) {
            auto& entry = loads[i];
            PluginLoadConfig cfg;

            cfg.name = toml::find<std::string>(entry, "name");
            cfg.enabled = entry.contains("enabled") ? toml::find<bool>(entry, "enabled") : true;

            if (!cfg.enabled) {
                g_log_queue.push("[INFO]  [插件] 跳过已禁用的插件: " + cfg.name);
                continue;
            }

            if (entry.contains("inline")) {
                cfg.inline_code = toml::find<std::string>(entry, "inline");
            } else if (entry.contains("file")) {
                cfg.file = toml::find<std::string>(entry, "file");
                bool found = false;
                for (auto& dir : dirs) {
                    std::string full_path = dir + "/" + cfg.file;
                    if (file_exists(full_path)) {
                        cfg.file = full_path;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    g_log_queue.push("[ERROR] [插件] 找不到文件: " + cfg.file);
                    failed++;
                    continue;
                }
            } else {
                g_log_queue.push("[ERROR] [插件] 插件项缺少 file 或 inline 字段: " + cfg.name);
                failed++;
                continue;
            }

            if (entry.contains("allow_exec")) {
                cfg.allow_exec = toml::find<bool>(entry, "allow_exec");
            }

            if (entry.contains("config")) {
                auto& cfg_toml = entry.at("config");
                for (auto& [k, v] : cfg_toml.as_table()) {
                    if (v.is_string()) cfg.config[k] = v.as_string();
                    else if (v.is_integer()) cfg.config[k] = (int64_t)v.as_integer();
                    else if (v.is_floating()) cfg.config[k] = v.as_floating();
                    else if (v.is_boolean()) cfg.config[k] = v.as_boolean();
                }
            }

            if (load_plugin(cfg)) {
                loaded++;
                loaded_names.insert(cfg.name);
            } else {
                failed++;
            }
        }
    }

    // === Phase 2: Auto-scan directories for .js files ===
    for (auto& dir : dirs) {
        DIR* dp = opendir(dir.c_str());
        if (!dp) {
            g_log_queue.push("[WARN]  [插件] 无法打开插件目录: " + dir);
            continue;
        }

        struct dirent* entry;
        while ((entry = readdir(dp)) != nullptr) {
            std::string name(entry->d_name);

            // Skip hidden files, non-.js files, and directories
            if (name.empty() || name[0] == '.' || name.size() < 4) continue;
            if (name.substr(name.size() - 3) != ".js") continue;

            std::string plugin_name = name.substr(0, name.size() - 3);

            // Skip if already loaded via [[PLUGINS.LOAD]]
            if (loaded_names.count(plugin_name)) continue;

            PluginLoadConfig cfg;
            cfg.name = plugin_name;
            cfg.file = dir + "/" + name;

            g_log_queue.push("[INFO]  [插件] 自动加载: " + plugin_name + " (" + cfg.file + ")");

            if (load_plugin(cfg)) {
                loaded++;
                loaded_names.insert(plugin_name);
            } else {
                failed++;
            }
        }
        closedir(dp);
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "[INFO]  [插件] 加载完成: %d 成功, %d 失败", loaded, failed);
    g_log_queue.push(buf);

    return failed == 0;
}

bool PluginManager::load_plugin(const PluginLoadConfig& cfg) {
    auto inst = std::make_unique<PluginInstance>();
    inst->name = cfg.name;
    inst->file = cfg.file;
    inst->enabled = cfg.enabled;
    inst->allow_exec = cfg.allow_exec;
    inst->config = cfg.config;
    inst->module_exports = JS_UNDEFINED;
    inst->on_before_check = JS_UNDEFINED;
    inst->on_after_check = JS_UNDEFINED;
    inst->on_item_approved = JS_UNDEFINED;
    inst->on_item_rejected = JS_UNDEFINED;
    inst->on_review_round_start = JS_UNDEFINED;
    inst->on_review_round_end = JS_UNDEFINED;
    inst->on_error = JS_UNDEFINED;
    inst->on_pause = JS_UNDEFINED;
    inst->on_resume = JS_UNDEFINED;

    // Create QuickJS runtime and context
    inst->rt = JS_NewRuntime();
    if (!inst->rt) {
        g_log_queue.push("[ERROR] [插件] 无法创建 JS 运行时: " + cfg.name);
        return false;
    }

    inst->ctx = JS_NewContext(inst->rt);
    if (!inst->ctx) {
        g_log_queue.push("[ERROR] [插件] 无法创建 JS 上下文: " + cfg.name);
        JS_FreeRuntime(inst->rt);
        inst->rt = nullptr;
        return false;
    }

    // Apply sandbox
    js_sandbox_apply(inst->ctx);

    // Set plugin context info (for bridge APIs to access)
    auto* ctx_info = new PluginContextInfo();
    ctx_info->plugin_name = cfg.name;
    ctx_info->data_dir = "./plugins/data/" + cfg.name;
    ctx_info->allow_exec = cfg.allow_exec;
    ctx_info->store = &inst->store;
    system(("mkdir -p " + ctx_info->data_dir).c_str());
    JS_SetContextOpaque(inst->ctx, ctx_info);

    // Remove dangerous globals
    js_sandbox_remove_dangerous(inst->ctx);

    // Register bridge APIs
    js_register_bridge_api(inst->ctx);

    // Initialize module.exports
    JSValue global = JS_GetGlobalObject(inst->ctx);
    JSValue module_obj = JS_NewObject(inst->ctx);
    JS_SetPropertyStr(inst->ctx, global, "module", module_obj);
    inst->module_exports = JS_UNDEFINED;
    JS_SetPropertyStr(inst->ctx, module_obj, "exports", JS_UNDEFINED);
    JS_FreeValue(inst->ctx, global);

    // Load the plugin code
    std::string js_code;
    if (!cfg.inline_code.empty()) {
        js_code = cfg.inline_code;
    } else {
        std::ifstream f(cfg.file);
        if (!f) {
            g_log_queue.push("[ERROR] [插件] 无法读取文件: " + cfg.file);
            JS_FreeContext(inst->ctx);
            JS_FreeRuntime(inst->rt);
            inst->ctx = nullptr;
            inst->rt = nullptr;
            return false;
        }
        js_code.assign((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    }

    // Evaluate the JS code
    JSEvalOptions eval_opts = {};
    eval_opts.version = JS_EVAL_OPTIONS_VERSION;
    eval_opts.eval_flags = JS_EVAL_TYPE_GLOBAL;
    std::string eval_filename = cfg.file.empty() ? ("inline:" + cfg.name) : cfg.file;
    eval_opts.filename = eval_filename.c_str();
    eval_opts.line_num = 1;

    JSValue eval_result = JS_Eval2(inst->ctx, js_code.c_str(), js_code.size(), &eval_opts);
    if (JS_IsException(eval_result)) {
        JsValue exc(inst->ctx, JS_GetException(inst->ctx));
        std::string err_msg = exc.to_string();
        g_log_queue.push("[ERROR] [插件] JS 执行错误: " + cfg.name + " - " + err_msg);
        JS_FreeValue(inst->ctx, eval_result);
        JS_FreeContext(inst->ctx);
        JS_FreeRuntime(inst->rt);
        inst->ctx = nullptr;
        inst->rt = nullptr;
        return false;
    }
    JS_FreeValue(inst->ctx, eval_result);

    // Extract module.exports
    global = JS_GetGlobalObject(inst->ctx);
    JSValue module_val = JS_GetPropertyStr(inst->ctx, global, "module");
    if (JS_IsObject(module_val)) {
        inst->module_exports = JS_GetPropertyStr(inst->ctx, module_val, "exports");
        JS_FreeValue(inst->ctx, module_val);
    } else {
        JS_FreeValue(inst->ctx, module_val);
        JS_FreeValue(inst->ctx, global);
        g_log_queue.push("[ERROR] [插件] 没有找到 module.exports: " + cfg.name);
        JS_FreeContext(inst->ctx);
        JS_FreeRuntime(inst->rt);
        inst->ctx = nullptr;
        inst->rt = nullptr;
        return false;
    }
    JS_FreeValue(inst->ctx, global);

    if (!JS_IsObject(inst->module_exports)) {
        g_log_queue.push("[ERROR] [插件] module.exports 不是一个对象: " + cfg.name);
        JS_FreeValue(inst->ctx, inst->module_exports);
        inst->module_exports = JS_UNDEFINED;
        JS_FreeContext(inst->ctx);
        JS_FreeRuntime(inst->rt);
        inst->ctx = nullptr;
        inst->rt = nullptr;
        return false;
    }

    // Extract hook functions
    extract_hook(inst.get(), "onLoad", inst->on_before_check); // not cached, called once
    extract_hook(inst.get(), "onBeforeCheck", inst->on_before_check);
    extract_hook(inst.get(), "onAfterCheck", inst->on_after_check);
    extract_hook(inst.get(), "onItemApproved", inst->on_item_approved);
    extract_hook(inst.get(), "onItemRejected", inst->on_item_rejected);
    extract_hook(inst.get(), "onReviewRoundStart", inst->on_review_round_start);
    extract_hook(inst.get(), "onReviewRoundEnd", inst->on_review_round_end);
    extract_hook(inst.get(), "onError", inst->on_error);
    extract_hook(inst.get(), "onPause", inst->on_pause);
    extract_hook(inst.get(), "onResume", inst->on_resume);

    // Call onLoad
    if (JS_IsObject(inst->module_exports)) {
        JSValue on_load = JS_GetPropertyStr(inst->ctx, inst->module_exports, "onLoad");
        if (JS_IsFunction(inst->ctx, on_load)) {
            // Create config object from nlohmann::json
            JSValue cfg_obj = JS_NewObject(inst->ctx);
            for (auto& [k, v] : cfg.config.items()) {
                if (v.is_string()) JS_SetPropertyStr(inst->ctx, cfg_obj, k.c_str(), JS_NewString(inst->ctx, v.get<std::string>().c_str()));
                else if (v.is_number_integer()) JS_SetPropertyStr(inst->ctx, cfg_obj, k.c_str(), JS_NewInt64(inst->ctx, v.get<int64_t>()));
                else if (v.is_number_float()) JS_SetPropertyStr(inst->ctx, cfg_obj, k.c_str(), JS_NewFloat64(inst->ctx, v.get<double>()));
                else if (v.is_boolean()) JS_SetPropertyStr(inst->ctx, cfg_obj, k.c_str(), JS_NewBool(inst->ctx, v.get<bool>()));
            }

            JSValue ret = JS_Call(inst->ctx, on_load, inst->module_exports, 1, &cfg_obj);
            if (JS_IsException(ret)) {
                JsValue exc(inst->ctx, JS_GetException(inst->ctx));
                g_log_queue.push("[ERROR] [插件] onLoad 错误: " + cfg.name + " - " + exc.to_string());
            }
            JS_FreeValue(inst->ctx, ret);
            JS_FreeValue(inst->ctx, cfg_obj);
        }
        JS_FreeValue(inst->ctx, on_load);
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "[INFO]  [插件] 已加载: %s", cfg.name.c_str());
    g_log_queue.push(buf);

    std::lock_guard<std::mutex> lk(mtx_);
    plugins_.push_back(std::move(inst));
    return true;
}

void PluginManager::unload_all() {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->ctx) continue;

        // Call onUnload if defined
        if (JS_IsObject(inst->module_exports)) {
            JSValue on_unload = JS_GetPropertyStr(inst->ctx, inst->module_exports, "onUnload");
            if (JS_IsFunction(inst->ctx, on_unload)) {
                JSValue ret = JS_Call(inst->ctx, on_unload, inst->module_exports, 0, nullptr);
                if (JS_IsException(ret)) {
                    JS_FreeValue(inst->ctx, JS_GetException(inst->ctx));
                }
                JS_FreeValue(inst->ctx, ret);
            }
            JS_FreeValue(inst->ctx, on_unload);
        }

        // Free cached hook references
        JS_FreeValue(inst->ctx, inst->on_before_check);
        JS_FreeValue(inst->ctx, inst->on_after_check);
        JS_FreeValue(inst->ctx, inst->on_item_approved);
        JS_FreeValue(inst->ctx, inst->on_item_rejected);
        JS_FreeValue(inst->ctx, inst->on_review_round_start);
        JS_FreeValue(inst->ctx, inst->on_review_round_end);
        JS_FreeValue(inst->ctx, inst->on_error);
        JS_FreeValue(inst->ctx, inst->on_pause);
        JS_FreeValue(inst->ctx, inst->on_resume);

        // Free module.exports
        JS_FreeValue(inst->ctx, inst->module_exports);

        // Free plugin context info
        auto* info = static_cast<PluginContextInfo*>(JS_GetContextOpaque(inst->ctx));
        delete info;
        JS_SetContextOpaque(inst->ctx, nullptr);

        // Free context and runtime
        JS_FreeContext(inst->ctx);
        inst->ctx = nullptr;
        JS_FreeRuntime(inst->rt);
        inst->rt = nullptr;
    }

    plugins_.clear();
}

void PluginManager::extract_hook(PluginInstance* inst, const char* hook_name, JSValue& output) {
    if (!inst->ctx || !JS_IsObject(inst->module_exports)) return;

    JSValue func = JS_GetPropertyStr(inst->ctx, inst->module_exports, hook_name);
    if (JS_IsFunction(inst->ctx, func)) {
        // If there's already a cached reference, free it
        if (!JS_IsUndefined(output)) {
            JS_FreeValue(inst->ctx, output);
        }
        output = func;  // Take ownership
    } else {
        JS_FreeValue(inst->ctx, func);
        output = JS_UNDEFINED;
    }
}

void PluginManager::call_hook_void(PluginInstance* inst, JSValue& func,
                                    int argc, JSValueConst* argv) {
    if (!inst || !inst->ctx || JS_IsUndefined(func) || JS_IsException(func)) return;

    JSValue ret = JS_Call(inst->ctx, func, inst->module_exports, argc, argv);
    if (JS_IsException(ret)) {
        JsValue exc(inst->ctx, JS_GetException(inst->ctx));
        std::string err = exc.to_string();
        if (!err.empty()) {
            g_log_queue.push("[ERROR] [插件:" + inst->name + "] " + err);
        }
    }
    JS_FreeValue(inst->ctx, ret);
}

JsCheckResult PluginManager::call_hook_check(PluginInstance* inst, JSValue& func,
                                              const std::string& text,
                                              const std::string& family_id,
                                              const std::string& type,
                                              const JsCheckResult* default_result) {
    JsCheckResult result;
    if (!inst || !inst->ctx || JS_IsUndefined(func) || JS_IsException(func)) return result;

    // Build arguments
    JSValue args[4];
    int argc = 0;

    args[argc++] = JS_NewString(inst->ctx, text.c_str());
    args[argc++] = JS_NewString(inst->ctx, family_id.c_str());
    args[argc++] = JS_NewString(inst->ctx, type.c_str());

    if (default_result) {
        JsObjectBuilder robj(inst->ctx);
        robj.set("shouldReject", default_result->should_reject);
        robj.set("reason", default_result->reason);
        args[argc++] = robj.build();
    }

    JSValue ret = JS_Call(inst->ctx, func, inst->module_exports, argc, args);

    // Free arg strings
    for (int i = 0; i < argc; ++i) {
        JS_FreeValue(inst->ctx, args[i]);
    }

    if (JS_IsException(ret)) {
        JsValue exc(inst->ctx, JS_GetException(inst->ctx));
        std::string err = exc.to_string();
        if (!err.empty()) {
            g_log_queue.push("[ERROR] [插件:" + inst->name + "] " + err);
        }
        JS_FreeValue(inst->ctx, ret);
        return result;
    }

    // Parse return value
    if (JS_IsObject(ret)) {
        JsValue reject_val(inst->ctx, JS_GetPropertyStr(inst->ctx, ret, "reject"), true);
        JsValue reason_val(inst->ctx, JS_GetPropertyStr(inst->ctx, ret, "reason"), true);

        if (reject_val.is_bool() && reject_val.to_bool()) {
            result.should_reject = true;
            result.reason = reason_val.to_string();
        }
    }

    JS_FreeValue(inst->ctx, ret);
    return result;
}

// ==================== Dispatchers ====================

JsCheckResult PluginManager::dispatch_before_check(const std::string& text,
                                                    const std::string& family_id,
                                                    const std::string& type) {
    std::lock_guard<std::mutex> lk(mtx_);
    JsCheckResult result;

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_before_check)) continue;

        JsCheckResult r = call_hook_check(inst.get(), inst->on_before_check,
                                          text, family_id, type);
        if (r.should_reject) {
            result = r;  // First plugin that rejects wins
            break;
        }
    }

    return result;
}

JsCheckResult PluginManager::dispatch_after_check(const std::string& text,
                                                   const JsCheckResult& default_result,
                                                   const std::string& family_id,
                                                   const std::string& type) {
    std::lock_guard<std::mutex> lk(mtx_);
    JsCheckResult result = default_result;

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_after_check)) continue;

        JsCheckResult r = call_hook_check(inst.get(), inst->on_after_check,
                                          text, family_id, type, &result);
        if (r.should_reject) {
            result = r;
        }
    }

    return result;
}

void PluginManager::dispatch_item_approved(const std::string& family_id,
                                           const std::string& type,
                                           const std::string& item_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_item_approved)) continue;

        JSValue args[3];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());
        args[1] = JS_NewString(inst->ctx, type.c_str());
        args[2] = JS_NewString(inst->ctx, item_id.c_str());

        call_hook_void(inst.get(), inst->on_item_approved, 3, args);

        JS_FreeValue(inst->ctx, args[0]);
        JS_FreeValue(inst->ctx, args[1]);
        JS_FreeValue(inst->ctx, args[2]);
    }
}

void PluginManager::dispatch_item_rejected(const std::string& family_id,
                                           const std::string& type,
                                           const std::string& item_id,
                                           const std::string& reason) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_item_rejected)) continue;

        JSValue args[4];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());
        args[1] = JS_NewString(inst->ctx, type.c_str());
        args[2] = JS_NewString(inst->ctx, item_id.c_str());
        args[3] = JS_NewString(inst->ctx, reason.c_str());

        call_hook_void(inst.get(), inst->on_item_rejected, 4, args);

        JS_FreeValue(inst->ctx, args[0]);
        JS_FreeValue(inst->ctx, args[1]);
        JS_FreeValue(inst->ctx, args[2]);
        JS_FreeValue(inst->ctx, args[3]);
    }
}

void PluginManager::dispatch_review_round_start(const std::string& family_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_review_round_start)) continue;

        JSValue args[1];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());

        call_hook_void(inst.get(), inst->on_review_round_start, 1, args);

        JS_FreeValue(inst->ctx, args[0]);
    }
}

void PluginManager::dispatch_review_round_end(const std::string& family_id,
                                              int total, int approved, int rejected) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_review_round_end)) continue;

        JSValue args[4];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());
        args[1] = JS_NewInt32(inst->ctx, total);
        args[2] = JS_NewInt32(inst->ctx, approved);
        args[3] = JS_NewInt32(inst->ctx, rejected);

        call_hook_void(inst.get(), inst->on_review_round_end, 4, args);

        JS_FreeValue(inst->ctx, args[0]);
        // Int32 values don't need freeing
    }
}

void PluginManager::dispatch_error(const std::string& family_id,
                                   const std::string& type,
                                   const std::string& error) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_error)) continue;

        JSValue args[3];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());
        args[1] = JS_NewString(inst->ctx, type.c_str());
        args[2] = JS_NewString(inst->ctx, error.c_str());

        call_hook_void(inst.get(), inst->on_error, 3, args);

        JS_FreeValue(inst->ctx, args[0]);
        JS_FreeValue(inst->ctx, args[1]);
        JS_FreeValue(inst->ctx, args[2]);
    }
}

void PluginManager::dispatch_pause(const std::string& family_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_pause)) continue;

        JSValue args[1];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());

        call_hook_void(inst.get(), inst->on_pause, 1, args);

        JS_FreeValue(inst->ctx, args[0]);
    }
}

void PluginManager::dispatch_resume(const std::string& family_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled) continue;
        if (JS_IsUndefined(inst->on_resume)) continue;

        JSValue args[1];
        args[0] = JS_NewString(inst->ctx, family_id.c_str());

        call_hook_void(inst.get(), inst->on_resume, 1, args);

        JS_FreeValue(inst->ctx, args[0]);
    }
}
