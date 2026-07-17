// config_types.h - Shared config types for auto_review
#pragma once

#include <string>
#include <vector>
#include <optional>

struct WebConfig {
    bool enabled = false;
    int port = 2356;
    std::string bind = "127.0.0.1";
    std::string root = "./web";
};

struct FamilyConfig {
    std::string family_id;
    std::string mid;
    bool enable_post = false;
    bool enable_comment = false;
    bool enable_join = false;
    bool enable_up = false;
    bool enable_up_resource = false;
    int min_level = -1;
    int max_up_resource_coin = -1;  // -1 表示使用全局配置
    std::vector<std::string> plugins;  // 该家族启用的插件列表，空表示使用全部
};

struct TokenConfig {
    std::string token;
    std::string uid;
    std::vector<FamilyConfig> families;
    std::optional<bool> enable_regex;
    std::optional<bool> enable_bad_words;
    std::optional<int> concurrency;
    std::optional<int> request_delay_ms;
};

struct ModuleConfig {
    bool enabled = false;
    std::string list_endpoint;
    std::string operate_endpoint;
    std::string state3_pending;
    std::string state3_approved;
    std::string state3_rejected;
    bool use_status3 = false;  // true 时用 status3 参数名代替 state3
};

struct Config {
    std::string sign_const;
    std::string base_url;
    std::string api_level;
    std::string version;
    std::string channel;
    std::string phone_model;
    std::string os_info;
    std::vector<TokenConfig> tokens;
    ModuleConfig post;
    ModuleConfig comment;
    ModuleConfig join;
    ModuleConfig up;
    ModuleConfig up_resource;
    std::string page;
    std::string limit;
    int check_interval_seconds = 300;
    int request_delay_ms = 100;
    int concurrency = 2;
    int max_up_resource_coin = -1;  // UP资源最大允许金币, -1 不限制
    std::string user_agent;
    bool enable_regex = false;
    bool tui_enabled = true;
    WebConfig web;
};
