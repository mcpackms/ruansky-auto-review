// js_bridge.h - C++ ↔ JS bridge API declarations
// All functions here are registered into the QuickJS global scope
#pragma once

#include <quickjs.h>
#include <string>
#include <nlohmann/json.hpp>

// Plugin context info stored per JS context via JS_SetContextOpaque
struct PluginContextInfo {
    std::string plugin_name;
    std::string data_dir;       // plugins/data/<plugin_name>/
    bool allow_exec = false;
    nlohmann::json* store = nullptr;  // pointer to plugin's KV store
};

// Register all bridge APIs into the given JS context
// This makes functions like log(), httpGet(), getConfig(), etc. available to JS plugins
void js_register_bridge_api(JSContext* ctx);
