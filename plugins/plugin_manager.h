// plugin_manager.h - Plugin manager for Node.js plugin system
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <toml.hpp>
#include <nlohmann/json.hpp>

// ==================== Check result type ====================
struct JsCheckResult {
    bool should_reject = false;
    std::string reason;
};

// ==================== Plugin configuration ====================
struct PluginLoadConfig {
    std::string name;
    std::string file;          // .js file path
    bool enabled = true;
    nlohmann::json config;     // custom config for the plugin
};

// ==================== Plugin instance ====================
struct PluginInstance {
    std::string name;
    std::string file;
    bool enabled = true;

    // Node.js child process
    pid_t pid = -1;
    int stdin_fd = -1;   // write to node stdin
    int stdout_fd = -1;  // read from node stdout

    // Custom config
    nlohmann::json config;

    // KV store (loaded from/saved to disk)
    nlohmann::json store;

    // File modification time, for change detection
    time_t file_mtime = 0;
};

// ==================== Plugin manager ====================
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    // Load plugins from TOML config
    bool load_from_config(const toml::value& plugins_config);

    // Load a single plugin
    bool load_plugin(const PluginLoadConfig& cfg);

    // Unload all plugins
    void unload_all();

    size_t count() const { return plugins_.size(); }
    bool empty() const { return plugins_.empty(); }

    // 设置某个家族启用哪些插件（空列表 = 使用全部）
    void set_family_plugins(const std::string& family_id,
                            const std::vector<std::string>& plugins);

    // 自动重载：扫描插件目录，新增/删除/更新已变更的插件
    void check_and_reload();

    // 启动/停止定时自动重载线程
    void start_auto_reload(int interval_seconds = 300);
    void stop_auto_reload();

    // ==================== Hook dispatchers ====================

    // Text check hooks (sync - wait for node response)
    JsCheckResult dispatch_before_check(const std::string& text,
                                        const std::string& family_id,
                                        const std::string& type);

    JsCheckResult dispatch_after_check(const std::string& text,
                                       const JsCheckResult& default_result,
                                       const std::string& family_id,
                                       const std::string& type);

    // Event hooks (async - fire and forget)
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
    // Spawn node process for a plugin instance
    bool spawn_node(PluginInstance* inst);

    // Send JSON command and wait for response (sync)
    // Returns parsed JSON response or nullptr on failure
    nlohmann::json send_command_sync(PluginInstance* inst,
                                     const nlohmann::json& cmd,
                                     int timeout_ms = 5000);

    // Send JSON command without waiting (async)
    void send_command_async(PluginInstance* inst, const nlohmann::json& cmd);

    // Close pipes and kill child process
    void close_plugin(PluginInstance* inst);

    // Readline helper: read one JSON line from fd with timeout
    std::string read_line(int fd, int timeout_ms);

    // 检查插件是否应分发到指定家族
    bool should_dispatch_to(const PluginInstance* inst,
                            const std::string& family_id) const;

    // 获取文件修改时间
    static time_t get_file_mtime(const std::string& path);

    // All loaded plugins
    std::vector<std::unique_ptr<PluginInstance>> plugins_;
    mutable std::mutex mtx_;
    int next_id_ = 1;

    // 插件搜索目录（从配置中保存）
    std::vector<std::string> dirs_;

    // 家族 -> 插件名列表，空 = 该家族使用全部插件
    std::unordered_map<std::string, std::set<std::string>> family_plugins_;

    // 自动重载线程控制
    std::atomic<bool> auto_reload_running_{false};
    std::thread auto_reload_thread_;
};

// Global plugin manager instance
extern PluginManager g_plugin_mgr;
