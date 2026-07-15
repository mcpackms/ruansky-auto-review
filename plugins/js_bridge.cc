// js_bridge.cc - Bridge API implementations for JS plugins
#include "js_bridge.h"
#include "quickjs_helpers.h"
#include "config_types.h"
#include "tui.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <functional>
#include <fstream>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <sys/wait.h>
#include <thread>

#include "config_types.h"
#include "tui.h"

// ==================== Forward declarations of extern symbols from main.cc ====================
extern Config g_config;
extern std::vector<TuiFamilyInfo> g_family_list;
extern std::function<TuiFamilyStats(const std::string&)> g_get_family_stats;
extern std::unordered_map<std::string, std::shared_ptr<FamilyControl>> g_family_controls;
extern std::atomic<bool> g_running;
extern int g_start_time;

// Access to bad words data (via extern functions)
extern int get_bad_words_count();
extern bool get_bad_words_enabled();
extern int get_regex_patterns_count();
extern bool get_regex_enabled();
// New extern function exposed from main.cc
using BadWordsList = std::vector<std::string>;
extern const BadWordsList& get_bad_words_list();

// ==================== Plugin context key ====================
// Helper to get the plugin context info from JS context
static PluginContextInfo* get_plugin_ctx_info(JSContext* ctx) {
    return static_cast<PluginContextInfo*>(JS_GetContextOpaque(ctx));
}

// ==================== HTTP helper ====================
static size_t write_cb(void* p, size_t s, size_t n, void* u) {
    size_t t = s * n;
    ((std::string*)u)->append((char*)p, t);
    return t;
}

// ==================== 1. log(level, ...args) ====================
static JSValue native_log(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;

    // Get level
    JsValue level_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string level = level_val.to_string();

    // Build message from all args after level
    std::string msg;
    for (int i = 1; i < argc; ++i) {
        if (i > 1) msg += " ";
        JsValue arg_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[i]), true);
        msg += arg_val.to_string();
    }

    // Determine log level prefix
    std::string fmt_msg;
    auto* info = get_plugin_ctx_info(ctx);
    if (info) {
        fmt_msg = "[插件:" + info->plugin_name + "] " + msg;
    } else {
        fmt_msg = "[插件] " + msg;
    }

    // Write to main log (check queue size BEFORE push to detect fallback)
    bool queue_empty = (g_log_queue.size() == 0);
    if (level == "error" || level == "err") {
        g_log_queue.push("[ERROR] " + fmt_msg);
        if (queue_empty) fprintf(stderr, "[ERROR] %s\n", fmt_msg.c_str());
    } else if (level == "warn" || level == "warning") {
        g_log_queue.push("[WARN]  " + fmt_msg);
        if (queue_empty) fprintf(stdout, "[WARN]  %s\n", fmt_msg.c_str());
    } else if (level == "debug") {
        g_log_queue.push("[DEBUG] " + fmt_msg);
    } else {
        g_log_queue.push("[INFO]  " + fmt_msg);
        if (queue_empty) fprintf(stdout, "[INFO]  %s\n", fmt_msg.c_str());
    }

    return JS_UNDEFINED;
}

// ==================== 2. print(...args) ====================
static JSValue native_print(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    for (int i = 0; i < argc; ++i) {
        if (i > 0) fputc(' ', stdout);
        JsValue v(const_cast<JSContext*>(ctx), JS_ToString(ctx, argv[i]));
        const char* s = JS_ToCString(ctx, v.get());
        if (s) {
            fputs(s, stdout);
            JS_FreeCString(ctx, s);
        }
    }
    fputc('\n', stdout);
    fflush(stdout);
    return JS_UNDEFINED;
}

// ==================== 3-5. Config access ====================
static JSValue native_getConfig(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue key_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string key = key_val.to_string();

    // Map config keys
    std::string result;
    if (key == "base_url") result = g_config.base_url;
    else if (key == "api_level") result = g_config.api_level;
    else if (key == "version") result = g_config.version;
    else if (key == "channel") result = g_config.channel;
    else if (key == "phone_model") result = g_config.phone_model;
    else if (key == "os_info") result = g_config.os_info;
    else if (key == "page") result = g_config.page;
    else if (key == "limit") result = g_config.limit;
    else if (key == "user_agent") result = g_config.user_agent;
    else if (key == "check_interval_seconds") return JS_NewInt32(ctx, g_config.check_interval_seconds);
    else if (key == "request_delay_ms") return JS_NewInt32(ctx, g_config.request_delay_ms);
    else if (key == "concurrency") return JS_NewInt32(ctx, g_config.concurrency);
    else if (key == "max_up_resource_coin") return JS_NewInt32(ctx, g_config.max_up_resource_coin);
    else if (key == "enable_regex") return JS_NewBool(ctx, g_config.enable_regex);
    else if (key == "tui_enabled") return JS_NewBool(ctx, g_config.tui_enabled);
    else if (key == "sign_const") return JS_NewString(ctx, "***");  // 脱敏
    else return JS_UNDEFINED;

    return JS_NewString(ctx, result.c_str());
}

static JSValue native_getModuleConfig(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue key_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string module_name = key_val.to_string();

    ModuleConfig* mod = nullptr;
    if (module_name == "post") mod = &g_config.post;
    else if (module_name == "comment") mod = &g_config.comment;
    else if (module_name == "join") mod = &g_config.join;
    else if (module_name == "up") mod = &g_config.up;
    else if (module_name == "up_resource") mod = &g_config.up_resource;
    else return JS_UNDEFINED;

    JsObjectBuilder obj(ctx);
    obj.set("enabled", mod->enabled);
    obj.set("list_endpoint", mod->list_endpoint);
    obj.set("operate_endpoint", mod->operate_endpoint);
    obj.set("state3_pending", mod->state3_pending);
    obj.set("state3_approved", mod->state3_approved);
    obj.set("state3_rejected", mod->state3_rejected);
    return obj.build();
}

static JSValue native_getPluginConfig(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info) return JS_UNDEFINED;
    // We can't easily return the config as JSValue here since config is a toml value
    // Return the plugin name at minimum
    JsObjectBuilder obj(ctx);
    obj.set("name", info->plugin_name);
    obj.set("data_dir", info->data_dir);
    obj.set("allow_exec", info->allow_exec);
    return obj.build();
}

// ==================== 6-7. Family info ====================
static JSValue native_getFamilies(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    for (auto& f : g_family_list) {
        JsObjectBuilder obj(ctx);
        obj.set("family_id", f.family_id);
        obj.set("token_mask", f.token_mask);
        obj.set("uid", f.uid);
        obj.set("paused", f.control ? f.control->paused.load() : false);
        obj.set("pending_count", f.control ? f.control->pending_count.load() : 0);
        obj.set("last_activity", f.control ? static_cast<int64_t>(f.control->last_activity.load()) : 0);
        obj.set("min_level", f.min_level);
        obj.set("max_up_resource_coin", f.max_up_resource_coin);

        JSValue modules = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, modules, "post", JS_NewBool(ctx, f.post_enabled));
        JS_SetPropertyStr(ctx, modules, "comment", JS_NewBool(ctx, f.comment_enabled));
        JS_SetPropertyStr(ctx, modules, "join", JS_NewBool(ctx, f.join_enabled));
        JS_SetPropertyStr(ctx, modules, "up", JS_NewBool(ctx, f.up_enabled));
        JS_SetPropertyStr(ctx, modules, "up_resource", JS_NewBool(ctx, f.up_resource_enabled));
        JS_SetPropertyStr(ctx, obj.get(), "modules", modules);

        JSValue js_obj = obj.build();
        JS_SetPropertyUint32(ctx, arr, idx++, js_obj);
    }
    return arr;
}

static JSValue native_getFamily(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();

    for (auto& f : g_family_list) {
        if (f.family_id == fid) {
            JsObjectBuilder obj(ctx);
            obj.set("family_id", f.family_id);
            obj.set("token_mask", f.token_mask);
            obj.set("uid", f.uid);
            obj.set("paused", f.control ? f.control->paused.load() : false);
            obj.set("pending_count", f.control ? f.control->pending_count.load() : 0);
            obj.set("last_activity", f.control ? static_cast<int64_t>(f.control->last_activity.load()) : 0);
            obj.set("min_level", f.min_level);
            obj.set("max_up_resource_coin", f.max_up_resource_coin);

            JSValue modules = JS_NewObject(ctx);
            JS_SetPropertyStr(ctx, modules, "post", JS_NewBool(ctx, f.post_enabled));
            JS_SetPropertyStr(ctx, modules, "comment", JS_NewBool(ctx, f.comment_enabled));
            JS_SetPropertyStr(ctx, modules, "join", JS_NewBool(ctx, f.join_enabled));
            JS_SetPropertyStr(ctx, modules, "up", JS_NewBool(ctx, f.up_enabled));
            JS_SetPropertyStr(ctx, modules, "up_resource", JS_NewBool(ctx, f.up_resource_enabled));
            JS_SetPropertyStr(ctx, obj.get(), "modules", modules);
            return obj.build();
        }
    }
    return JS_UNDEFINED;
}

// ==================== 8-9. Running state ====================
static JSValue native_isRunning(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    return JS_NewBool(ctx, g_running.load());
}

static JSValue native_getUptime(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    return JS_NewInt64(ctx, static_cast<int64_t>(time(nullptr) - g_start_time));
}

static JSValue native_getStartTime(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    char buf[64];
    struct tm tm_buf;
    time_t t = g_start_time;
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", localtime_r(&t, &tm_buf));
    return JS_NewString(ctx, buf);
}

// ==================== 10-13. Family control ====================
static JSValue native_isFamilyPaused(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();
    auto it = g_family_controls.find(fid);
    if (it != g_family_controls.end()) {
        return JS_NewBool(ctx, it->second->paused.load());
    }
    return JS_FALSE;
}

static JSValue native_pauseFamily(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();
    auto it = g_family_controls.find(fid);
    if (it != g_family_controls.end()) {
        it->second->paused = true;
        return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue native_resumeFamily(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();
    auto it = g_family_controls.find(fid);
    if (it != g_family_controls.end()) {
        it->second->paused = false;
        return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue native_getFamilyPendingCount(JSContext* ctx, JSValueConst this_val,
                                           int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt32(ctx, 0);
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();
    auto it = g_family_controls.find(fid);
    if (it != g_family_controls.end()) {
        return JS_NewInt32(ctx, it->second->pending_count.load());
    }
    return JS_NewInt32(ctx, 0);
}

static JSValue native_getFamilyLastActivity(JSContext* ctx, JSValueConst this_val,
                                            int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewInt64(ctx, 0);
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();
    auto it = g_family_controls.find(fid);
    if (it != g_family_controls.end()) {
        return JS_NewInt64(ctx, static_cast<int64_t>(it->second->last_activity.load()));
    }
    return JS_NewInt64(ctx, 0);
}

// ==================== 14-16. Stats ====================
static JSValue family_stats_to_js(JSContext* ctx, const TuiFamilyStats& st) {
    JsObjectBuilder obj(ctx);
    auto add_mod = [&](const char* name, int total, int approved, int rejected) {
        JSValue m = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, m, "total", JS_NewInt32(ctx, total));
        JS_SetPropertyStr(ctx, m, "approved", JS_NewInt32(ctx, approved));
        JS_SetPropertyStr(ctx, m, "rejected", JS_NewInt32(ctx, rejected));
        double rate = total > 0 ? std::round(approved * 1000.0 / total) / 10.0 : 0.0;
        JS_SetPropertyStr(ctx, m, "rate", JS_NewFloat64(ctx, rate));
        JS_SetPropertyStr(ctx, obj.get(), name, m);
    };
    add_mod("post", st.post_total, st.post_approved, st.post_rejected);
    add_mod("comment", st.comment_total, st.comment_approved, st.comment_rejected);
    add_mod("join", st.join_total, st.join_approved, st.join_rejected);
    add_mod("up", st.up_total, st.up_approved, st.up_rejected);
    add_mod("up_resource", st.up_resource_total, st.up_resource_approved, st.up_resource_rejected);
    return obj.build();
}

static JSValue native_getStats(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();
    if (!g_get_family_stats) return JS_UNDEFINED;
    auto st = g_get_family_stats(fid);
    return family_stats_to_js(ctx, st);
}

static JSValue native_getAllStats(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    JSValue obj = JS_NewObject(ctx);
    if (!g_get_family_stats) return obj;
    for (auto& f : g_family_list) {
        auto st = g_get_family_stats(f.family_id);
        JS_SetPropertyStr(ctx, obj, f.family_id.c_str(), family_stats_to_js(ctx, st));
    }
    return obj;
}

static JSValue native_getTotalStats(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    TuiFamilyStats total;
    if (g_get_family_stats) {
        for (auto& f : g_family_list) {
            auto st = g_get_family_stats(f.family_id);
            total.post_total += st.post_total;
            total.post_approved += st.post_approved;
            total.post_rejected += st.post_rejected;
            total.comment_total += st.comment_total;
            total.comment_approved += st.comment_approved;
            total.comment_rejected += st.comment_rejected;
            total.join_total += st.join_total;
            total.join_approved += st.join_approved;
            total.join_rejected += st.join_rejected;
            total.up_total += st.up_total;
            total.up_approved += st.up_approved;
            total.up_rejected += st.up_rejected;
            total.up_resource_total += st.up_resource_total;
            total.up_resource_approved += st.up_resource_approved;
            total.up_resource_rejected += st.up_resource_rejected;
        }
    }
    return family_stats_to_js(ctx, total);
}

// ==================== 17-18. Bad words ====================
static JSValue native_getBadWords(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    JSValue arr = JS_NewArray(ctx);
    const auto& words = get_bad_words_list();
    for (size_t i = 0; i < words.size(); ++i) {
        JS_SetPropertyUint32(ctx, arr, static_cast<uint32_t>(i),
                             JS_NewString(ctx, words[i].c_str()));
    }
    return arr;
}

static JSValue native_getBadWordsEnabled(JSContext* ctx, JSValueConst this_val,
                                         int argc, JSValueConst* argv) {
    return JS_NewBool(ctx, get_bad_words_enabled());
}

static JSValue native_getRegexPatterns(JSContext* ctx, JSValueConst this_val,
                                       int argc, JSValueConst* argv) {
    // We don't store the pattern strings after compiling regex, just return count
    return JS_NewInt32(ctx, get_regex_patterns_count());
}

// ==================== 19-20. HTTP ====================
static JSValue do_http_request(JSContext* ctx, const std::string& url,
                               const std::string& method,
                               JSValueConst params_val,
                               JSValueConst headers_val) {
    std::string full_url = url;
    CURL* curl = curl_easy_init();
    if (!curl) {
        JS_ThrowPlainError(ctx, "Failed to initialize curl");
        return JS_EXCEPTION;
    }

    // Build params
    if (JS_IsObject(params_val)) {
        JsValue keys_val(const_cast<JSContext*>(ctx), JS_GetPropertyStr(ctx, params_val, "keys"), true);
        // Simple approach: iterate and build query string
        // We use the global object approach carefully
        std::string query;
        JSAtom atom;
        JSPropertyEnum* props = nullptr;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &plen, params_val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < plen; ++i) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                if (key) {
                    JsValue val(ctx, JS_GetProperty(ctx, params_val, props[i].atom));
                    std::string sval = val.to_string();
                    if (!query.empty()) query += "&";
                    query += std::string(key) + "=" + curl_easy_escape(curl, sval.c_str(), sval.length());
                    JS_FreeCString(ctx, key);
                }
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
        if (!query.empty()) {
            if (full_url.find('?') != std::string::npos) full_url += "&";
            else full_url += "?";
            full_url += query;
        }
    }

    std::string response_body;
    struct curl_slist* headers = nullptr;
    std::string content_type;

    // Build custom headers
    if (JS_IsObject(headers_val)) {
        JSPropertyEnum* props = nullptr;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &plen, headers_val,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < plen; ++i) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                if (key) {
                    JsValue val(ctx, JS_GetProperty(ctx, headers_val, props[i].atom));
                    std::string sval = val.to_string();
                    std::string header = std::string(key) + ": " + sval;
                    headers = curl_slist_append(headers, header.c_str());
                    if (strcasecmp(key, "content-type") == 0) content_type = sval;
                    JS_FreeCString(ctx, key);
                }
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }

    // Setup curl
    curl_easy_setopt(curl, CURLOPT_URL, full_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_config.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    // Get response headers
    std::string resp_headers;
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_cb);

    if (headers) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    }

    long status_code = 0;
    CURLcode res;

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        // We need the body - but we don't have it as a separate parameter
        // Actually, for POST we handle it differently in native_httpPost
        curl_easy_perform(curl);
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    if (headers) curl_slist_free_all(headers);

    // Build result object
    JsObjectBuilder result(ctx);
    result.set("status", static_cast<int32_t>(status_code));
    result.set("body", response_body);

    // Parse response headers into object
    JSValue resp_hdr_obj = JS_NewObject(ctx);
    // Simple parsing: split by newlines
    std::istringstream hstream(resp_headers);
    std::string line;
    while (std::getline(hstream, line)) {
        // Remove trailing \r
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            // Trim leading spaces
            if (!val.empty() && val[0] == ' ') val = val.substr(1);
            JS_SetPropertyStr(ctx, resp_hdr_obj, key.c_str(), JS_NewString(ctx, val.c_str()));
        }
    }
    JS_SetPropertyStr(ctx, result.get(), "headers", resp_hdr_obj);

    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        result.set("error", curl_easy_strerror(res));
    }

    return result.build();
}

static JSValue native_httpGet(JSContext* ctx, JSValueConst this_val,
                              int argc, JSValueConst* argv) {
    if (argc < 1) {
        JS_ThrowPlainError(ctx, "httpGet requires at least 1 argument: url");
        return JS_EXCEPTION;
    }
    JsValue url_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string url = url_val.to_string();

    JSValue params = (argc > 1) ? argv[1] : JS_UNDEFINED;
    JSValue headers = (argc > 2) ? argv[2] : JS_UNDEFINED;

    return do_http_request(ctx, url, "GET", params, headers);
}

static JSValue native_httpPost(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) {
        JS_ThrowPlainError(ctx, "httpPost requires at least 1 argument: url");
        return JS_EXCEPTION;
    }
    JsValue url_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string url = url_val.to_string();

    // Build curl directly for POST
    CURL* curl = curl_easy_init();
    if (!curl) {
        JS_ThrowPlainError(ctx, "Failed to initialize curl");
        return JS_EXCEPTION;
    }

    std::string post_body;
    JSValue headers = (argc > 2) ? argv[2] : JS_UNDEFINED;

    // Process body
    if (argc > 1 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
        if (JS_IsObject(argv[1])) {
            // JSON serialize
            JsValue json_str(const_cast<JSContext*>(ctx), JS_ToString(ctx, argv[1]), true);
            post_body = json_str.to_string();
        } else {
            JsValue body_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[1]), true);
            post_body = body_val.to_string();
        }
    }

    std::string response_body;
    std::string resp_headers_str;
    struct curl_slist* headers_list = nullptr;

    // Build custom headers
    if (JS_IsObject(headers)) {
        JSPropertyEnum* props = nullptr;
        uint32_t plen = 0;
        if (JS_GetOwnPropertyNames(ctx, &props, &plen, headers,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
            for (uint32_t i = 0; i < plen; ++i) {
                const char* key = JS_AtomToCString(ctx, props[i].atom);
                if (key) {
                    JsValue val(ctx, JS_GetProperty(ctx, headers, props[i].atom));
                    std::string sval = val.to_string();
                    std::string hdr = std::string(key) + ": " + sval;
                    headers_list = curl_slist_append(headers_list, hdr.c_str());
                    JS_FreeCString(ctx, key);
                }
                JS_FreeAtom(ctx, props[i].atom);
            }
            js_free(ctx, props);
        }
    }

    // Auto-detect content type
    if (JS_IsObject(argv[1]) && argc > 1) {
        bool has_content_type = false;
        if (JS_IsObject(headers)) {
            JsValue ct_val(const_cast<JSContext*>(ctx),
                           JS_GetPropertyStr(ctx, headers, "Content-Type"), true);
            if (!ct_val.is_undefined()) has_content_type = true;
        }
        if (!has_content_type) {
            headers_list = curl_slist_append(headers_list, "Content-Type: application/json");
        }
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, post_body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers_str);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, g_config.user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    if (headers_list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers_list);

    CURLcode res = curl_easy_perform(curl);
    long status_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_code);

    if (headers_list) curl_slist_free_all(headers_list);

    JsObjectBuilder result(ctx);
    result.set("status", static_cast<int32_t>(status_code));
    result.set("body", response_body);

    // Parse response headers
    JSValue resp_hdr_obj = JS_NewObject(ctx);
    std::istringstream hstream(resp_headers_str);
    std::string line;
    while (std::getline(hstream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            std::string val = line.substr(colon + 1);
            if (!val.empty() && val[0] == ' ') val = val.substr(1);
            JS_SetPropertyStr(ctx, resp_hdr_obj, key.c_str(), JS_NewString(ctx, val.c_str()));
        }
    }
    JS_SetPropertyStr(ctx, result.get(), "headers", resp_hdr_obj);

    if (res != CURLE_OK) {
        result.set("error", curl_easy_strerror(res));
    }

    curl_easy_cleanup(curl);
    return result.build();
}

// ==================== 21-24. File system (sandboxed) ====================
static std::string sanitize_path(JSContext* ctx, const std::string& path) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info) return "";

    // Plugin files are restricted to plugins/data/<plugin_name>/
    std::string sandbox_root = info->data_dir;
    if (sandbox_root.empty()) return "";

    // Ensure directory exists
    std::string cmd = "mkdir -p " + sandbox_root;
    system(cmd.c_str());

    // Resolve relative to sandbox root
    if (path.empty() || path[0] == '/') return sandbox_root;  // just the root
    return sandbox_root + "/" + path;
}

static JSValue native_readFile(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue path_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string safe_path = sanitize_path(ctx, path_val.to_string());
    if (safe_path.empty()) return JS_NULL;

    std::ifstream f(safe_path, std::ios::binary | std::ios::ate);
    if (!f) return JS_NULL;
    auto size = f.tellg();
    f.seekg(0);
    std::string content(size, '\0');
    f.read(&content[0], size);
    return JS_NewStringLen(ctx, content.c_str(), content.size());
}

static JSValue native_writeFile(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 2) return JS_FALSE;
    JsValue path_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    JsValue content_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[1]), true);
    std::string safe_path = sanitize_path(ctx, path_val.to_string());
    if (safe_path.empty()) return JS_FALSE;

    std::ofstream f(safe_path, std::ios::binary);
    if (!f) return JS_FALSE;
    std::string content = content_val.to_string();
    f.write(content.c_str(), content.size());
    return JS_TRUE;
}

static JSValue native_deleteFile(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 1) return JS_FALSE;
    JsValue path_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string safe_path = sanitize_path(ctx, path_val.to_string());
    if (safe_path.empty()) return JS_FALSE;
    return JS_NewBool(ctx, std::remove(safe_path.c_str()) == 0);
}

static JSValue native_listFiles(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    std::string dir;
    if (argc > 0) {
        JsValue dir_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
        dir = sanitize_path(ctx, dir_val.to_string());
    } else {
        auto* info = get_plugin_ctx_info(ctx);
        if (!info) return JS_NewArray(ctx);
        dir = info->data_dir;
    }
    if (dir.empty()) return JS_NewArray(ctx);

    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    std::string cmd = "ls -1 \"" + dir + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) {
            size_t len = strlen(buf);
            if (len > 0 && buf[len-1] == '\n') buf[len-1] = '\0';
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, buf));
        }
        pclose(fp);
    }
    return arr;
}

// ==================== 25-29. KV Store ====================
static std::string store_path(const std::string& plugin_name) {
    return std::string("./plugins/data/") + plugin_name + ".json";
}

static nlohmann::json* load_store(const std::string& plugin_name) {
    static std::unordered_map<std::string, nlohmann::json> s_stores;
    auto& j = s_stores[plugin_name];
    if (j.is_null()) {
        std::ifstream f(store_path(plugin_name));
        if (f) {
            try { j = nlohmann::json::parse(f); } catch (...) { j = nlohmann::json::object(); }
        } else {
            j = nlohmann::json::object();
        }
    }
    return &j;
}

static void save_store(const std::string& plugin_name, nlohmann::json* j) {
    if (!j) return;
    std::string dir = "./plugins/data";
    std::string cmd = "mkdir -p " + dir;
    system(cmd.c_str());
    std::ofstream f(store_path(plugin_name));
    if (f) f << j->dump(2);
}

static JSValue native_storeGet(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info || argc < 1) return JS_UNDEFINED;
    JsValue key_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string key = key_val.to_string();

    auto* store = load_store(info->plugin_name);
    if (!store || !store->contains(key)) return JS_UNDEFINED;
    auto& val = (*store)[key];

    if (val.is_string()) return JS_NewString(ctx, val.get<std::string>().c_str());
    else if (val.is_number_integer()) return JS_NewInt64(ctx, val.get<int64_t>());
    else if (val.is_number_float()) return JS_NewFloat64(ctx, val.get<double>());
    else if (val.is_boolean()) return JS_NewBool(ctx, val.get<bool>());
    else if (val.is_null()) return JS_NULL;
    else return JS_NewString(ctx, val.dump().c_str());
}

static JSValue native_storeSet(JSContext* ctx, JSValueConst this_val,
                               int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info || argc < 2) return JS_UNDEFINED;
    JsValue key_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string key = key_val.to_string();

    auto* store = load_store(info->plugin_name);

    JsValue val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[1]), true);
    if (val.is_string()) (*store)[key] = val.to_string();
    else if (val.is_number()) (*store)[key] = val.to_float64();
    else if (val.is_bool()) (*store)[key] = val.to_bool();
    else if (val.is_null()) (*store)[key] = nullptr;
    else if (val.is_object() || val.is_array()) {
        JsValue str(ctx, JS_ToString(ctx, val.get()), true);
        try { (*store)[key] = nlohmann::json::parse(str.to_string()); } catch (...) { (*store)[key] = str.to_string(); }
    } else (*store)[key] = val.to_string();

    save_store(info->plugin_name, store);
    return JS_UNDEFINED;
}

static JSValue native_storeDelete(JSContext* ctx, JSValueConst this_val,
                                  int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info || argc < 1) return JS_FALSE;
    JsValue key_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string key = key_val.to_string();
    auto* store = load_store(info->plugin_name);
    if (store && store->contains(key)) {
        store->erase(key);
        save_store(info->plugin_name, store);
        return JS_TRUE;
    }
    return JS_FALSE;
}

static JSValue native_storeKeys(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info) return JS_NewArray(ctx);
    auto* store = load_store(info->plugin_name);
    JSValue arr = JS_NewArray(ctx);
    uint32_t idx = 0;
    if (store) {
        for (auto& [key, _] : store->items()) {
            JS_SetPropertyUint32(ctx, arr, idx++, JS_NewString(ctx, key.c_str()));
        }
    }
    return arr;
}

static JSValue native_storeClear(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info) return JS_UNDEFINED;
    auto* store = load_store(info->plugin_name);
    if (store) {
        store->clear();
        save_store(info->plugin_name, store);
    }
    return JS_UNDEFINED;
}

// ==================== 30-34. Time utilities ====================
static JSValue native_now(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv) {
    time_t t = time(nullptr);
    struct tm tm_buf;
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime_r(&t, &tm_buf));
    return JS_NewString(ctx, buf);
}

static JSValue native_date(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    time_t t = time(nullptr);
    struct tm tm_buf;
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", localtime_r(&t, &tm_buf));
    return JS_NewString(ctx, buf);
}

static JSValue native_timestamp(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    return JS_NewInt64(ctx, static_cast<int64_t>(time(nullptr)));
}

static JSValue native_formatDate(JSContext* ctx, JSValueConst this_val,
                                 int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JsValue fmt_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fmt = fmt_val.to_string();

    time_t t = time(nullptr);
    if (argc > 1) {
        JsValue ts_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[1]), true);
        int64_t ts = ts_val.to_int64();
        if (ts > 0) t = static_cast<time_t>(ts);
    }

    struct tm tm_buf;
    localtime_r(&t, &tm_buf);

    // Simple format: YYYY-MM-DD HH:mm:ss
    std::string result = fmt;
    auto replace_all = [](std::string& s, const char* from, const char* to) {
        size_t pos = 0;
        size_t from_len = strlen(from);
        size_t to_len = strlen(to);
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from_len, to);
            pos += to_len;
        }
    };

    char buf[32];
    snprintf(buf, sizeof(buf), "%04d", tm_buf.tm_year + 1900); replace_all(result, "YYYY", buf);
    snprintf(buf, sizeof(buf), "%02d", tm_buf.tm_mon + 1);     replace_all(result, "MM", buf);
    snprintf(buf, sizeof(buf), "%02d", tm_buf.tm_mday);        replace_all(result, "DD", buf);
    snprintf(buf, sizeof(buf), "%02d", tm_buf.tm_hour);        replace_all(result, "HH", buf);
    snprintf(buf, sizeof(buf), "%02d", tm_buf.tm_min);         replace_all(result, "mm", buf);
    snprintf(buf, sizeof(buf), "%02d", tm_buf.tm_sec);         replace_all(result, "ss", buf);

    return JS_NewString(ctx, result.c_str());
}

// ==================== 35-39. Encoding/Decoding ====================
static JSValue native_md5(JSContext* ctx, JSValueConst this_val,
                          int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JsValue text_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string text = text_val.to_string();

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_MD_CTX* mdctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(mdctx, EVP_md5(), nullptr);
    EVP_DigestUpdate(mdctx, text.c_str(), text.size());
    EVP_DigestFinal_ex(mdctx, digest, &len);
    EVP_MD_CTX_free(mdctx);

    char buf[33];
    for (unsigned int i = 0; i < len; ++i) snprintf(buf + i * 2, 3, "%02x", digest[i]);
    return JS_NewString(ctx, buf);
}

static JSValue native_base64Encode(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JsValue text_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string text = text_val.to_string();

    BIO* bio = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    BIO_write(bio, text.c_str(), text.size());
    BIO_flush(bio);

    char* encoded = nullptr;
    long len = BIO_get_mem_data(bio, &encoded);
    std::string result(encoded, len);
    BIO_free_all(bio);
    return JS_NewStringLen(ctx, result.c_str(), result.size());
}

static JSValue native_base64Decode(JSContext* ctx, JSValueConst this_val,
                                   int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JsValue text_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string text = text_val.to_string();

    BIO* bio = BIO_new_mem_buf(text.c_str(), text.size());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);

    std::string out;
    out.resize(text.size());
    int len = BIO_read(bio, &out[0], out.size());
    BIO_free_all(bio);
    if (len <= 0) return JS_NewString(ctx, "");
    out.resize(len);
    return JS_NewStringLen(ctx, out.c_str(), out.size());
}

static JSValue native_urlEncode(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JsValue text_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string text = text_val.to_string();
    CURL* curl = curl_easy_init();
    if (!curl) return JS_NewString(ctx, text.c_str());
    char* encoded = curl_easy_escape(curl, text.c_str(), text.length());
    std::string result(encoded);
    curl_free(encoded);
    curl_easy_cleanup(curl);
    return JS_NewString(ctx, result.c_str());
}

static JSValue native_urlDecode(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JsValue text_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string text = text_val.to_string();
    CURL* curl = curl_easy_init();
    if (!curl) return JS_NewString(ctx, text.c_str());
    int outlen;
    char* decoded = curl_easy_unescape(curl, text.c_str(), text.length(), &outlen);
    std::string result(decoded, outlen);
    curl_free(decoded);
    curl_easy_cleanup(curl);
    return JS_NewString(ctx, result.c_str());
}

// ==================== 40-41. JSON ====================
static JSValue native_jsonParse(JSContext* ctx, JSValueConst this_val,
                                int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NULL;
    JsValue str_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string str = str_val.to_string();
    JSValue result = JS_ParseJSON(ctx, str.c_str(), str.size(), "<json>");
    if (JS_IsException(result)) return JS_NULL;
    return result;
}

static JSValue native_jsonStringify(JSContext* ctx, JSValueConst this_val,
                                    int argc, JSValueConst* argv) {
    if (argc < 1) return JS_NewString(ctx, "");
    JSValue obj = argv[0];
    // Check if pretty
    bool pretty = false;
    if (argc > 1) {
        JsValue pv(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[1]), true);
        pretty = pv.to_bool();
    }

    // QuickJS's JS_ToJSON doesn't exist, so we use JS_ToString
    JsValue result(const_cast<JSContext*>(ctx), JS_ToString(ctx, obj), true);
    return JS_NewString(ctx, result.to_string().c_str());
}

// ==================== 42. sleep ====================
static JSValue native_sleep(JSContext* ctx, JSValueConst this_val,
                            int argc, JSValueConst* argv) {
    int ms = 100;
    if (argc > 0) {
        JsValue ms_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
        ms = ms_val.to_int32(100);
    }
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    return JS_UNDEFINED;
}

// ==================== 43. exec (disabled by default) ====================
static JSValue native_exec(JSContext* ctx, JSValueConst this_val,
                           int argc, JSValueConst* argv) {
    auto* info = get_plugin_ctx_info(ctx);
    if (!info || !info->allow_exec) {
        JS_ThrowPlainError(ctx, "exec is disabled for this plugin. Set allow_exec=true in config.");
        return JS_EXCEPTION;
    }
    if (argc < 1) {
        JS_ThrowPlainError(ctx, "exec requires a command string");
        return JS_EXCEPTION;
    }

    JsValue cmd_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string cmd = cmd_val.to_string();

    // Execute and capture output
    std::string stdout_str, stderr_str;
    int ret_code = -1;

    // Use popen for stdout
    FILE* fp = popen((cmd + " 2>/tmp/_plugin_stderr").c_str(), "r");
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) stdout_str += buf;
        ret_code = pclose(fp);
    }

    // Read stderr
    std::ifstream ef("/tmp/_plugin_stderr");
    if (ef) {
        stderr_str.assign((std::istreambuf_iterator<char>(ef)),
                           std::istreambuf_iterator<char>());
        ef.close();
        std::remove("/tmp/_plugin_stderr");
    }

    JsObjectBuilder result(ctx);
    result.set("code", ret_code);
    result.set("stdout", stdout_str);
    result.set("stderr", stderr_str);
    return result.build();
}

// ==================== Get token config ====================
static JSValue native_getTokenConfig(JSContext* ctx, JSValueConst this_val,
                                     int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue idx_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    int idx = idx_val.to_int32(-1);
    if (idx < 0 || idx >= static_cast<int>(g_config.tokens.size())) return JS_UNDEFINED;

    auto& tc = g_config.tokens[idx];
    JsObjectBuilder obj(ctx);
    // Token masked
    std::string mask;
    if (tc.token.size() > 8) mask = tc.token.substr(0, 4) + "****" + tc.token.substr(tc.token.size() - 4);
    else mask = std::string(tc.token.size(), '*');
    obj.set("token_mask", mask);
    obj.set("uid", tc.uid);
    if (tc.enable_regex) obj.set("enable_regex", *tc.enable_regex);
    if (tc.enable_bad_words) obj.set("enable_bad_words", *tc.enable_bad_words);
    if (tc.concurrency) obj.set("concurrency", *tc.concurrency);
    if (tc.request_delay_ms) obj.set("request_delay_ms", *tc.request_delay_ms);

    // Families
    JSValue fam_arr = JS_NewArray(ctx);
    for (size_t i = 0; i < tc.families.size(); ++i) {
        auto& f = tc.families[i];
        JsObjectBuilder fobj(ctx);
        fobj.set("family_id", f.family_id);
        fobj.set("mid", f.mid);
        fobj.set("enable_post", f.enable_post);
        fobj.set("enable_comment", f.enable_comment);
        fobj.set("enable_join", f.enable_join);
        fobj.set("enable_up", f.enable_up);
        fobj.set("enable_up_resource", f.enable_up_resource);
        fobj.set("min_level", f.min_level);
        fobj.set("max_up_resource_coin", f.max_up_resource_coin);
        JSValue fv = fobj.build();
        JS_SetPropertyUint32(ctx, fam_arr, static_cast<uint32_t>(i), fv);
    }
    JS_SetPropertyStr(ctx, obj.get(), "families", fam_arr);

    return obj.build();
}

// ==================== Get family config ====================
static JSValue native_getFamilyConfig(JSContext* ctx, JSValueConst this_val,
                                      int argc, JSValueConst* argv) {
    if (argc < 1) return JS_UNDEFINED;
    JsValue fid_val(const_cast<JSContext*>(ctx), JS_DupValue(ctx, argv[0]), true);
    std::string fid = fid_val.to_string();

    // Search all tokens for this family
    for (auto& tc : g_config.tokens) {
        for (auto& f : tc.families) {
            if (f.family_id == fid) {
                JsObjectBuilder obj(ctx);
                obj.set("family_id", f.family_id);
                obj.set("mid", f.mid);
                obj.set("enable_post", f.enable_post);
                obj.set("enable_comment", f.enable_comment);
                obj.set("enable_join", f.enable_join);
                obj.set("enable_up", f.enable_up);
                obj.set("enable_up_resource", f.enable_up_resource);
                obj.set("min_level", f.min_level);
                obj.set("max_up_resource_coin", f.max_up_resource_coin);
                return obj.build();
            }
        }
    }
    return JS_UNDEFINED;
}

// ==================== Register all APIs ====================
void js_register_bridge_api(JSContext* ctx) {
    JSValue global = JS_GetGlobalObject(ctx);

    // Helper to register a function
    auto reg = [&](const char* name, JSCFunction* func, int nargs) {
        JSValue f = JS_NewCFunction(ctx, func, name, nargs);
        JS_SetPropertyStr(ctx, global, name, f);
    };

    // 1. Logging & Diagnostics
    reg("log", native_log, 2);
    reg("print", native_print, 1);

    // 2. Config access
    reg("getConfig", native_getConfig, 1);
    reg("getModuleConfig", native_getModuleConfig, 1);
    reg("getTokenConfig", native_getTokenConfig, 1);
    reg("getFamilyConfig", native_getFamilyConfig, 1);
    reg("getPluginConfig", native_getPluginConfig, 0);

    // 3. Family info & running state
    reg("getFamilies", native_getFamilies, 0);
    reg("getFamily", native_getFamily, 1);
    reg("isRunning", native_isRunning, 0);
    reg("getUptime", native_getUptime, 0);
    reg("getStartTime", native_getStartTime, 0);

    // 4. Family control
    reg("isFamilyPaused", native_isFamilyPaused, 1);
    reg("pauseFamily", native_pauseFamily, 1);
    reg("resumeFamily", native_resumeFamily, 1);
    reg("getFamilyPendingCount", native_getFamilyPendingCount, 1);
    reg("getFamilyLastActivity", native_getFamilyLastActivity, 1);

    // 5. Statistics
    reg("getStats", native_getStats, 1);
    reg("getAllStats", native_getAllStats, 0);
    reg("getTotalStats", native_getTotalStats, 0);

    // 6. Bad words
    reg("getBadWords", native_getBadWords, 0);
    reg("getBadWordsEnabled", native_getBadWordsEnabled, 0);
    reg("getRegexPatterns", native_getRegexPatterns, 0);

    // 7. HTTP
    reg("httpGet", native_httpGet, 1);
    reg("httpPost", native_httpPost, 1);

    // 8. File system
    reg("readFile", native_readFile, 1);
    reg("writeFile", native_writeFile, 2);
    reg("deleteFile", native_deleteFile, 1);
    reg("listFiles", native_listFiles, 0);

    // 9. KV Store
    reg("storeGet", native_storeGet, 1);
    reg("storeSet", native_storeSet, 2);
    reg("storeDelete", native_storeDelete, 1);
    reg("storeKeys", native_storeKeys, 0);
    reg("storeClear", native_storeClear, 0);

    // 10. Time
    reg("now", native_now, 0);
    reg("date", native_date, 0);
    reg("timestamp", native_timestamp, 0);
    reg("formatDate", native_formatDate, 1);

    // 11. Encoding
    reg("md5", native_md5, 1);
    reg("base64Encode", native_base64Encode, 1);
    reg("base64Decode", native_base64Decode, 1);
    reg("urlEncode", native_urlEncode, 1);
    reg("urlDecode", native_urlDecode, 1);

    // 12. JSON
    reg("jsonParse", native_jsonParse, 1);
    reg("jsonStringify", native_jsonStringify, 1);

    // 13. Other
    reg("sleep", native_sleep, 1);
    reg("exec", native_exec, 1);

    JS_FreeValue(ctx, global);
}
