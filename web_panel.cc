// web_panel.cc - Web management panel implementation
// Copyright (C) 2026 YIZHIDIANBI (一支电笔)
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published
// by the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "web_panel.h"
#include "tui.h"
#include "httplib.h"
#include <cstdio>
#include <functional>
#include <cstring>
#include <nlohmann/json.hpp>

#include "config_types.h"

#include <atomic>
#include <ctime>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>


// ==================== 从 main.cc 引用的全局变量 ====================
extern std::atomic<bool> g_running;
extern std::atomic<bool> g_once_mode;
extern std::atomic<bool> g_no_tui;
extern Theme g_theme;

// 从 main.cc 引用的配置
extern Config g_config;

// 从 main.cc 导出的辅助函数（避开 DoubleArrayAC 等复杂类型）
extern int get_bad_words_count();
extern bool get_bad_words_enabled();
extern int get_regex_patterns_count();
extern bool get_regex_enabled();

// 从 main.cc 引用的统计回调
extern std::function<TuiFamilyStats(const std::string&)> g_get_family_stats;

// 从 main.cc 引用
extern int g_start_time;

std::time_t g_web_start_time = 0;

// ==================== 辅助函数 ====================

static std::string theme_to_string(Theme t) {
    switch (t) {
        case Theme::TOKYO_NIGHT: return "tokyo-night";
        case Theme::DARK: return "dark";
        case Theme::LIGHT: return "light";
        default: return "tokyo-night";
    }
}

static std::string mode_to_string() {
    if (g_no_tui) return "no-tui";
    if (g_once_mode) return "once";
    return "tui";
}

static double calc_rate(int approved, int total) {
    if (total <= 0) return 0.0;
    return std::round(approved * 1000.0 / total) / 10.0;
}

// ==================== 路由设置 ====================

void run_web_server(int port, const std::string& static_dir, const std::string& bind_addr) {
    g_web_start_time = time(nullptr);
    httplib::Server svr;

    // ---- CORS 支持 ----
    svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
        res.set_header("Access-Control-Allow-Origin", "*");
        res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        res.set_header("Access-Control-Allow-Headers", "Content-Type");
        if (req.method == "OPTIONS") {
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });

    // ---- GET /api/status ----
    svr.Get("/api/status", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json j;
        j["running"] = g_running.load();
        j["uptime"] = static_cast<int>(time(nullptr) - g_web_start_time);
        j["mode"] = mode_to_string();
        j["theme"] = theme_to_string(g_theme);
        j["version"] = "1.3.3";
        j["started_at"] = g_web_start_time;

        // 配置摘要
        j["config"]["concurrency"] = g_config.concurrency;
        j["config"]["request_delay_ms"] = g_config.request_delay_ms;
        j["config"]["check_interval_seconds"] = g_config.check_interval_seconds;
        j["config"]["enable_regex"] = g_config.enable_regex;
        j["config"]["bad_words_count"] = get_bad_words_count();
        j["config"]["bad_words_enabled"] = get_bad_words_enabled();
        j["config"]["regex_patterns_count"] = get_regex_patterns_count();
        j["config"]["regex_enabled"] = get_regex_enabled();
        j["config"]["post_enabled"] = g_config.post.enabled;
        j["config"]["comment_enabled"] = g_config.comment.enabled;
        j["config"]["join_enabled"] = g_config.join.enabled;
        j["config"]["up_enabled"] = g_config.up.enabled;
        j["config"]["up_resource_enabled"] = g_config.up_resource.enabled;
        j["config"]["max_up_resource_coin"] = g_config.max_up_resource_coin;

        // 家族统计
        int online = 0;
        for (auto& info : g_family_list) {
            if (info.control && !info.control->paused.load()) online++;
        }
        j["families_total"] = static_cast<int>(g_family_list.size());
        j["families_online"] = online;

        res.set_content(j.dump(), "application/json");
    });

    // ---- GET /api/families ----
    svr.Get("/api/families", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json arr = nlohmann::json::array();

        for (auto& info : g_family_list) {
            nlohmann::json f;
            f["family_id"] = info.family_id;
            f["token_mask"] = info.token_mask;
            // UID 脱敏处理
            {
                std::string masked_uid = info.uid;
                if (masked_uid.size() > 4) {
                    masked_uid = masked_uid.substr(0, 2) + "***" + masked_uid.substr(masked_uid.size() - 2);
                }
                f["uid"] = masked_uid;
            }
            f["paused"] = info.control ? info.control->paused.load() : false;
            f["last_activity"] = info.control ? static_cast<int>(info.control->last_activity.load()) : 0;
            f["min_level"] = info.min_level;
            f["max_up_resource_coin"] = info.max_up_resource_coin;

            f["modules"]["post"] = info.post_enabled;
            f["modules"]["comment"] = info.comment_enabled;
            f["modules"]["join"] = info.join_enabled;
            f["modules"]["up"] = info.up_enabled;
            f["modules"]["up_resource"] = info.up_resource_enabled;

            auto st = g_get_family_stats ? g_get_family_stats(info.family_id) : TuiFamilyStats();

            auto add_module = [&](const std::string& name, int total, int approved, int rejected) {
                f["stats"][name]["total"] = total;
                f["stats"][name]["approved"] = approved;
                f["stats"][name]["rejected"] = rejected;
                f["stats"][name]["rate"] = calc_rate(approved, total);
            };

            add_module("post",        st.post_total,        st.post_approved,        st.post_rejected);
            add_module("comment",     st.comment_total,     st.comment_approved,     st.comment_rejected);
            add_module("join",        st.join_total,        st.join_approved,        st.join_rejected);
            add_module("up",          st.up_total,          st.up_approved,          st.up_rejected);
            add_module("up_resource", st.up_resource_total, st.up_resource_approved, st.up_resource_rejected);

            arr.push_back(f);
        }

        res.set_content(arr.dump(), "application/json");
    });

    // ---- GET /api/family/:id ----
    svr.Get(R"(/api/family/(\d+))", [](const httplib::Request& req, httplib::Response& res) {
        std::string fid = req.matches[1];
        nlohmann::json result;

        for (auto& info : g_family_list) {
            if (info.family_id == fid) {
                result["family_id"] = info.family_id;
                result["token_mask"] = info.token_mask;
                // UID 脱敏
                {
                    std::string masked = info.uid;
                    if (masked.size() > 4) {
                        masked = masked.substr(0, 2) + "***" + masked.substr(masked.size() - 2);
                    }
                    result["uid"] = masked;
                }
                result["paused"] = info.control ? info.control->paused.load() : false;
                result["pending_count"] = info.control ? info.control->pending_count.load() : 0;
                result["last_activity"] = info.control ? static_cast<int>(info.control->last_activity.load()) : 0;
                result["min_level"] = info.min_level;
                result["max_up_resource_coin"] = info.max_up_resource_coin;
                result["modules"]["post"] = info.post_enabled;
                result["modules"]["comment"] = info.comment_enabled;
                result["modules"]["join"] = info.join_enabled;
                result["modules"]["up"] = info.up_enabled;
                result["modules"]["up_resource"] = info.up_resource_enabled;

                auto st = g_get_family_stats ? g_get_family_stats(info.family_id) : TuiFamilyStats();
                auto add_module = [&](const std::string& name, int total, int approved, int rejected) {
                    result["stats"][name]["total"] = total;
                    result["stats"][name]["approved"] = approved;
                    result["stats"][name]["rejected"] = rejected;
                    result["stats"][name]["rate"] = calc_rate(approved, total);
                };
                add_module("post",        st.post_total,        st.post_approved,        st.post_rejected);
                add_module("comment",     st.comment_total,     st.comment_approved,     st.comment_rejected);
                add_module("join",        st.join_total,        st.join_approved,        st.join_rejected);
                add_module("up",          st.up_total,          st.up_approved,          st.up_rejected);
                add_module("up_resource", st.up_resource_total, st.up_resource_approved, st.up_resource_rejected);

                res.set_content(result.dump(), "application/json");
                return;
            }
        }

        res.status = 404;
        nlohmann::json err;
        err["error"] = "Family not found";
        res.set_content(err.dump(), "application/json");
    });

    // ---- GET /api/logs ----
    svr.Get("/api/logs", [](const httplib::Request& req, httplib::Response& res) {
        std::string fid = req.has_param("fid") ? req.get_param_value("fid") : "";
        std::string count_str = req.has_param("count") ? req.get_param_value("count") : "50";
        int count = 50;
        try { count = std::stoi(count_str); } catch (...) {}
        if (count < 1) count = 1;
        if (count > 500) count = 500;

        nlohmann::json result;
        result["total"] = static_cast<int>(g_log_queue.size());

        if (fid.empty()) {
            auto entries = g_log_queue.get_tail(count);
            nlohmann::json logs = nlohmann::json::array();
            for (size_t i = 0; i < entries.size(); ++i) {
                nlohmann::json entry;
                entry["index"] = static_cast<int>(result["total"].get<int>() - entries.size() + i);
                entry["msg"] = entries[i];
                logs.push_back(entry);
            }
            result["logs"] = logs;
        } else {
            auto entries = g_log_queue.get_family_logs(fid, count);
            nlohmann::json logs = nlohmann::json::array();
            for (auto& [idx, msg] : entries) {
                nlohmann::json entry;
                entry["index"] = static_cast<int>(idx);
                entry["msg"] = msg;
                logs.push_back(entry);
            }
            result["logs"] = logs;
        }

        res.set_content(result.dump(), "application/json");
    });

    // ---- GET /api/config ----
    svr.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        nlohmann::json j;

        // 全局配置
        j["global"]["sign_const"] = "***"; // 脱敏
        j["global"]["base_url"] = g_config.base_url;
        j["global"]["api_level"] = g_config.api_level;
        j["global"]["version"] = g_config.version;
        j["global"]["channel"] = g_config.channel;
        j["global"]["phone_model"] = g_config.phone_model;
        j["global"]["os_info"] = g_config.os_info;
        j["global"]["page"] = g_config.page;
        j["global"]["limit"] = g_config.limit;
        j["global"]["check_interval_seconds"] = g_config.check_interval_seconds;
        j["global"]["request_delay_ms"] = g_config.request_delay_ms;
        j["global"]["concurrency"] = g_config.concurrency;
        j["global"]["user_agent"] = g_config.user_agent;
        j["global"]["enable_regex"] = g_config.enable_regex;

        // Token 配置（脱敏）
        nlohmann::json tokens = nlohmann::json::array();
        for (auto& tc : g_config.tokens) {
            nlohmann::json t;
            std::string mask;
            if (tc.token.size() > 8) {
                mask = tc.token.substr(0, 4) + "****" + tc.token.substr(tc.token.size() - 4);
            } else {
                mask = std::string(tc.token.size(), '*');
            }
            t["token_mask"] = mask;
            // UID 脱敏
            {
                std::string muid = tc.uid;
                if (muid.size() > 4) {
                    muid = muid.substr(0, 2) + "***" + muid.substr(muid.size() - 2);
                }
                t["uid"] = muid;
            }
            if (tc.enable_regex) t["enable_regex"] = *tc.enable_regex;
            if (tc.enable_bad_words) t["enable_bad_words"] = *tc.enable_bad_words;
            if (tc.concurrency) t["concurrency"] = *tc.concurrency;
            if (tc.request_delay_ms) t["request_delay_ms"] = *tc.request_delay_ms;

            nlohmann::json families = nlohmann::json::array();
            for (auto& f : tc.families) {
                nlohmann::json fam;
                fam["family_id"] = f.family_id;
                fam["mid"] = f.mid;
                fam["enable_post"] = f.enable_post;
                fam["enable_comment"] = f.enable_comment;
                fam["enable_join"] = f.enable_join;
                fam["enable_up"] = f.enable_up;
                fam["min_level"] = f.min_level;
                families.push_back(fam);
            }
            t["families"] = families;
            tokens.push_back(t);
        }
        j["tokens"] = tokens;

        res.set_content(j.dump(), "application/json");
    });

    // ---- 静态文件 ----
    svr.set_base_dir(static_dir);

    // ---- 启动 ----
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "🌐\tWeb 管理面板: http://%s:%d\t端口: %d", bind_addr.c_str(), port, port);
        g_log_queue.push(std::string("[INFO] ") + buf);
        printf("%s\n", buf);
        fflush(stdout);
    }

    if (bind_addr == "0.0.0.0") {
        char buf[256];
        snprintf(buf, sizeof(buf), "📡\t绑定: %s:%d", bind_addr.c_str(), port);
        g_log_queue.push(std::string("[INFO] ") + buf);
        printf("%s\n", buf);
        fflush(stdout);
    }

    svr.listen(bind_addr, port);
}
