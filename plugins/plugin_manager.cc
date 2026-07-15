// plugin_manager.cc - Plugin manager for Node.js plugin system
#include "plugin_manager.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <set>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "config_types.h"
#include "tui.h"

// ==================== Global plugin manager ====================
PluginManager g_plugin_mgr;

extern std::unordered_map<std::string, std::shared_ptr<FamilyControl>> g_family_controls;

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

// Find "node" executable
static std::string find_node() {
    // First check common locations
    static const char* candidates[] = {
        "/usr/bin/node",
        "/usr/local/bin/node",
        "/data/data/com.termux/files/usr/bin/node",
        "/opt/homebrew/bin/node",
        nullptr
    };
    for (int i = 0; candidates[i]; ++i) {
        if (file_exists(candidates[i])) return candidates[i];
    }
    // Fall back to PATH lookup
    const char* path = getenv("PATH");
    if (path) {
        std::string p(path);
        size_t start = 0;
        while (true) {
            size_t end = p.find(':', start);
            std::string dir = p.substr(start, end - start);
            std::string candidate = dir + "/node";
            if (file_exists(candidate)) return candidate;
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return "node";  // fallback, let execvp find it
}

// ==================== PluginManager ====================

PluginManager::PluginManager() {
    // Ensure data directory exists
    mkdir("./plugins/data", 0755);
}

PluginManager::~PluginManager() {
    unload_all();
}

bool PluginManager::load_from_config(const toml::value& plugins_config) {
    std::vector<std::string> dirs = {"./plugins"};

    if (plugins_config.contains("dirs")) {
        dirs.clear();
        auto& dirs_arr = plugins_config.at("dirs").as_array();
        for (auto& d : dirs_arr) {
            dirs.push_back(d.as_string());
        }
    }

    mkdir("./plugins/data", 0755);

    int loaded = 0;
    int failed = 0;
    std::set<std::string> loaded_names;

    // Phase 1: Load explicitly configured plugins ([[PLUGINS.LOAD]])
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

            if (entry.contains("file")) {
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
                g_log_queue.push("[ERROR] [插件] 插件项缺少 file 字段: " + cfg.name);
                failed++;
                continue;
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

    // Phase 2: Auto-scan directories for .js files
    for (auto& dir : dirs) {
        DIR* dp = opendir(dir.c_str());
        if (!dp) {
            g_log_queue.push("[WARN]  [插件] 无法打开插件目录: " + dir);
            continue;
        }

        struct dirent* entry;
        while ((entry = readdir(dp)) != nullptr) {
            std::string name(entry->d_name);
            if (name.empty() || name[0] == '.' || name.size() < 4) continue;
            if (name.substr(name.size() - 3) != ".js") continue;

            std::string plugin_name = name.substr(0, name.size() - 3);
            if (plugin_name == "node_host") continue;  // skip the host script
            if (loaded_names.count(plugin_name)) continue;

            PluginLoadConfig cfg;
            cfg.name = plugin_name;
            cfg.file = dir + "/" + name;

            g_log_queue.push("[INFO]  [插件] 自动加载: " + plugin_name);
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
    inst->config = cfg.config;

    if (!spawn_node(inst.get())) {
        g_log_queue.push("[ERROR] [插件] 无法启动子进程: " + cfg.name);
        return false;
    }

    // Wait for ready message from node
    std::string ready_line = read_line(inst->stdout_fd, 5000);
    if (ready_line.empty()) {
        g_log_queue.push("[ERROR] [插件] 启动超时: " + cfg.name);
        close_plugin(inst.get());
        return false;
    }
    // Send load command
    nlohmann::json load_cmd;
    load_cmd["id"] = 0;
    load_cmd["cmd"] = "load";
    load_cmd["file"] = cfg.file;
    load_cmd["config"] = cfg.config;

    nlohmann::json resp = send_command_sync(inst.get(), load_cmd, 10000);
    if (resp.is_null() || !resp.value("ok", false)) {
        std::string err = resp.value("error", "未知错误");
        g_log_queue.push("[ERROR] [插件] 加载失败: " + cfg.name + " - " + err);
        close_plugin(inst.get());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        plugins_.push_back(std::move(inst));
    }

    g_log_queue.push("[INFO]  [插件] 已加载: " + cfg.name);
    return true;
}

void PluginManager::unload_all() {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (inst && inst->pid > 0) {
            // Send shutdown command
            nlohmann::json cmd;
            cmd["id"] = -1;
            cmd["cmd"] = "shutdown";
            send_command_async(inst.get(), cmd);

            // Wait briefly for graceful shutdown
            int status;
            pid_t ret = waitpid(inst->pid, &status, WNOHANG);
            if (ret == 0) {
                // Not exited yet, wait a bit
                for (int i = 0; i < 20; ++i) {
                    usleep(50000);  // 50ms intervals, up to 1s
                    ret = waitpid(inst->pid, &status, WNOHANG);
                    if (ret != 0) break;
                }
            }
            if (ret == 0) {
                // Force kill
                kill(inst->pid, SIGKILL);
                waitpid(inst->pid, &status, 0);
            }
        }
        close_plugin(inst.get());
    }

    plugins_.clear();
}

// ==================== Process management ====================

bool PluginManager::spawn_node(PluginInstance* inst) {
    // Find node_host.js path
    std::string host_script = "./plugins/node_host.js";
    if (!file_exists(host_script)) {
        // Try alternative path
        host_script = "plugins/node_host.js";
        if (!file_exists(host_script)) {
            g_log_queue.push("[ERROR] [插件] 找不到 node_host.js");
            return false;
        }
    }

    // Find node executable
    std::string node_exe = find_node();

    // Ensure data dir exists
    std::string data_dir = "./plugins/data/" + inst->name;
    mkdir(data_dir.c_str(), 0755);

    // Create pipes
    int stdin_pipe[2];  // parent -> child stdin
    int stdout_pipe[2]; // child stdout -> parent

    if (pipe(stdin_pipe) < 0 || pipe(stdout_pipe) < 0) {
        g_log_queue.push("[ERROR] [插件] 管道创建失败");
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        g_log_queue.push("[ERROR] [插件] fork 失败");
        close(stdin_pipe[0]);
        close(stdin_pipe[1]);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // Child process
        close(stdin_pipe[1]);   // close write end of stdin pipe
        close(stdout_pipe[0]);  // close read end of stdout pipe

        // Redirect stdin/stdout
        dup2(stdin_pipe[0], STDIN_FILENO);
        dup2(stdout_pipe[1], STDOUT_FILENO);

        close(stdin_pipe[0]);
        close(stdout_pipe[1]);

        // Execute node
        const char* argv[] = {
            node_exe.c_str(),
            host_script.c_str(),
            inst->file.c_str(),
            inst->name.c_str(),
            data_dir.c_str(),
            nullptr
        };

        execvp(node_exe.c_str(), (char* const*)argv);

        // If exec fails
        fprintf(stderr, "[插件] exec 失败: %s\n", strerror(errno));
        _exit(1);
    }

    // Parent process
    close(stdin_pipe[0]);   // close read end
    close(stdout_pipe[1]);  // close write end

    inst->pid = pid;
    inst->stdin_fd = stdin_pipe[1];
    inst->stdout_fd = stdout_pipe[0];

    // Set stdout to non-blocking for async reads
    int flags = fcntl(inst->stdout_fd, F_GETFL, 0);
    fcntl(inst->stdout_fd, F_SETFL, flags | O_NONBLOCK);

    return true;
}

void PluginManager::close_plugin(PluginInstance* inst) {
    if (inst->stdin_fd >= 0) {
        close(inst->stdin_fd);
        inst->stdin_fd = -1;
    }
    if (inst->stdout_fd >= 0) {
        close(inst->stdout_fd);
        inst->stdout_fd = -1;
    }
    inst->pid = -1;
}

// ==================== IPC helpers ====================

std::string PluginManager::read_line(int fd, int timeout_ms) {
    std::string line;
    char buf[4096];
    int pos = 0;

    while (true) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) {
            // Timeout or error
            return "";
        }

        ssize_t n = read(fd, buf + pos, sizeof(buf) - pos - 1);
        if (n <= 0) {
            // EOF or error
            return "";
        }

        // Check for newline in the newly read data
        for (ssize_t i = 0; i < n; ++i) {
            if (buf[pos + i] == '\n') {
                buf[pos + i] = '\0';
                line = std::string(buf);
                return line;
            }
        }

        pos += n;
        if (pos >= (int)sizeof(buf) - 1) {
            buf[pos] = '\0';
            line = std::string(buf);
            return line;
        }
    }
}

nlohmann::json PluginManager::send_command_sync(PluginInstance* inst,
                                                 const nlohmann::json& cmd,
                                                 int timeout_ms) {
    if (inst->stdin_fd < 0 || inst->stdout_fd < 0) {
        return nlohmann::json();
    }

    // Write command to stdin
    std::string cmd_str = cmd.dump() + "\n";
    ssize_t written = write(inst->stdin_fd, cmd_str.c_str(), cmd_str.size());
    if (written < 0 || (size_t)written != cmd_str.size()) {
        return nlohmann::json();
    }

    // Read response from stdout
    // We need to read until we get a complete JSON object on a line
    std::string response_line = read_line(inst->stdout_fd, timeout_ms);
    if (response_line.empty()) {
        return nlohmann::json();
    }

    try {
        return nlohmann::json::parse(response_line);
    } catch (...) {
        return nlohmann::json();
    }
}

void PluginManager::send_command_async(PluginInstance* inst,
                                        const nlohmann::json& cmd) {
    if (inst->stdin_fd < 0) return;

    std::string cmd_str = cmd.dump() + "\n";
    write(inst->stdin_fd, cmd_str.c_str(), cmd_str.size());
    // Don't wait for response
}

// ==================== Build command helper ====================

static nlohmann::json make_cmd(int id, const std::string& hook,
                                const std::vector<std::string>& args) {
    nlohmann::json cmd;
    cmd["id"] = id;
    cmd["cmd"] = "call";
    cmd["hook"] = hook;
    cmd["args"] = nlohmann::json::array();
    for (auto& a : args) {
        cmd["args"].push_back(a);
    }
    return cmd;
}

// ==================== Dispatchers ====================

JsCheckResult PluginManager::dispatch_before_check(const std::string& text,
                                                    const std::string& family_id,
                                                    const std::string& type) {
    std::lock_guard<std::mutex> lk(mtx_);
    JsCheckResult result;

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;

        auto cmd = make_cmd(next_id_++, "onBeforeCheck", {text, family_id, type});
        auto resp = send_command_sync(inst.get(), cmd);

        if (!resp.is_null() && resp.value("ok", false)) {
            auto& r = resp["result"];
            if (!r.is_null() && r.is_object()) {
                bool reject = r.value("shouldReject", false);
                if (reject) {
                    result.should_reject = true;
                    result.reason = r.value("reason", "");
                    break;  // first rejection wins
                }
            }
        }
    }

    return result;
}

JsCheckResult PluginManager::dispatch_after_check(const std::string& text,
                                                   const JsCheckResult& default_result,
                                                   const std::string& family_id,
                                                   const std::string& type) {
    std::lock_guard<std::mutex> lk(mtx_);
    JsCheckResult result;

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;

        auto cmd = make_cmd(next_id_++, "onAfterCheck",
                            {text, family_id, type,
                             default_result.should_reject ? "true" : "false",
                             default_result.reason});
        auto resp = send_command_sync(inst.get(), cmd);

        if (!resp.is_null() && resp.value("ok", false)) {
            auto& r = resp["result"];
            if (!r.is_null() && r.is_object()) {
                bool reject = r.value("shouldReject", false);
                if (reject) {
                    result.should_reject = true;
                    result.reason = r.value("reason", "");
                    break;
                }
            }
        }
    }

    return result;
}

void PluginManager::dispatch_item_approved(const std::string& family_id,
                                            const std::string& type,
                                            const std::string& item_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onItemApproved", {family_id, type, item_id});
        send_command_async(inst.get(), cmd);
    }
}

void PluginManager::dispatch_item_rejected(const std::string& family_id,
                                            const std::string& type,
                                            const std::string& item_id,
                                            const std::string& reason) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onItemRejected", {family_id, type, item_id, reason});
        send_command_async(inst.get(), cmd);
    }
}

void PluginManager::dispatch_review_round_start(const std::string& family_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onReviewRoundStart", {family_id});
        send_command_async(inst.get(), cmd);
    }
}

void PluginManager::dispatch_review_round_end(const std::string& family_id,
                                               int total, int approved, int rejected) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onReviewRoundEnd", {family_id});
        cmd["args"] = {family_id, total, approved, rejected};
        send_command_async(inst.get(), cmd);
    }
}

void PluginManager::dispatch_error(const std::string& family_id,
                                    const std::string& type,
                                    const std::string& error) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onError", {family_id, type, error});
        send_command_async(inst.get(), cmd);
    }
}

void PluginManager::dispatch_pause(const std::string& family_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onPause", {family_id});
        send_command_async(inst.get(), cmd);
    }
}

void PluginManager::dispatch_resume(const std::string& family_id) {
    std::lock_guard<std::mutex> lk(mtx_);

    for (auto& inst : plugins_) {
        if (!inst || !inst->enabled || inst->pid <= 0) continue;
        auto cmd = make_cmd(0, "onResume", {family_id});
        send_command_async(inst.get(), cmd);
    }
}
