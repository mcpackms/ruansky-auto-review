// plugin_manager.h - Plugin manager for JS plugin system
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <functional>

#include <quickjs.h>
#include <toml.hpp>
#include <nlohmann/json.hpp>

#include "quickjs_helpers.h"

// ==================== Plugin configuration ====================
struct PluginLoadConfig {
    std::string name;
    std::string file;          // .js file path (relative to plugins dirs)
    std::string inline_code;   // inline JS code (alternative to file)
    bool enabled = true;
    bool allow_exec = false;   // allow exec() API
    nlohmann::json config;     // custom config for the plugin
};

// ==================== Plugin instance ====================
struct PluginInstance {
    std::string name;
    std::string file;
    bool enabled = true;
    bool allow_exec = false;

    // QuickJS runtime and context (isolated per plugin)
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;

    // Module.exports reference
    JSValue module_exports = JS_UNDEFINED;

    // Cached hook function references
    JSValue on_before_check = JS_UNDEFINED;
    JSValue on_after_check = JS_UNDEFINED;
    JSValue on_item_approved = JS_UNDEFINED;
    JSValue on_item_rejected = JS_UNDEFINED;
    JSValue on_review_round_start = JS_UNDEFINED;
    JSValue on_review_round_end = JS_UNDEFINED;
    JSValue on_error = JS_UNDEFINED;
    JSValue on_pause = JS_UNDEFINED;
    JSValue on_resume = JS_UNDEFINED;

    // Custom config (from settings.toml [PLUGINS.LOAD.config])
    nlohmann::json config;

    // KV store reference
    nlohmann::json store;
};

// ==================== Plugin manager ====================
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load plugins from TOML config
    // Expected format:
    //   [PLUGINS]
    //   dirs = ["./plugins"]
    //   [[PLUGINS.LOAD]]
    //   name = "my_plugin"
    //   file = "my_plugin.js"
    //   enabled = true
    //   [PLUGINS.LOAD.config]
    //   key = "value"
    bool load_from_config(const toml::value& plugins_config);

    // Load a single plugin
    bool load_plugin(const PluginLoadConfig& cfg);

    // Unload all plugins
    void unload_all();

    // Get number of loaded plugins
    size_t count() const { return plugins_.size(); }

    // Check if any plugins are loaded
    bool empty() const { return plugins_.empty(); }

    // ==================== Hook dispatchers ====================
    // These are called from main.cc at the appropriate points

    // Text check hooks (hot path - must be fast)
    // Returns JsCheckResult which may override the default check
    JsCheckResult dispatch_before_check(const std::string& text,
                                        const std::string& family_id,
                                        const std::string& type);

    JsCheckResult dispatch_after_check(const std::string& text,
                                       const JsCheckResult& default_result,
                                       const std::string& family_id,
                                       const std::string& type);

    // Event hooks (fire-and-forget, non-critical path)
    void dispatch_item_approved(const std::string& family_id,
                                const std::string& type,
                                const std::string& item_id);

    void dispatch_item_rejected(const std::string& family_id,
                                const std::string& type,
                                const std::string& item_id,
                                const std::string& reason);

    void dispatch_review_round_start(const std::string& family_id);
    void dispatch_review_round_end(const std::string& family_id,
                                   int total, int approved, int rejected);

    void dispatch_error(const std::string& family_id,
                        const std::string& type,
                        const std::string& error);

    void dispatch_pause(const std::string& family_id);
    void dispatch_resume(const std::string& family_id);

private:
    // Internal: load a JS file into a context and extract hooks
    bool init_plugin_instance(PluginInstance* inst, const PluginLoadConfig& cfg);

    // Internal: extract hook function reference from module.exports
    // Stores the cached JSValue into the provided output reference
    void extract_hook(PluginInstance* inst, const char* hook_name, JSValue& output);

    // Internal: safely call a JS hook function with no return value
    void call_hook_void(PluginInstance* inst, JSValue& func,
                        int argc, JSValueConst* argv);

    // Internal: safely call a JS hook function that returns a JsCheckResult
    JsCheckResult call_hook_check(PluginInstance* inst, JSValue& func,
                                  const std::string& text,
                                  const std::string& family_id,
                                  const std::string& type,
                                  const JsCheckResult* default_result = nullptr);

    // All loaded plugins
    std::vector<std::unique_ptr<PluginInstance>> plugins_;
    mutable std::mutex mtx_;
};

// Global plugin manager instance
extern PluginManager g_plugin_mgr;
