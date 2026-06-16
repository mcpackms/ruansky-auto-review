// tui.h - TUI header for auto_review
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

#pragma once

#include <atomic>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// ==================== 线程安全日志队列 ====================
class LogQueue {
    mutable std::mutex mtx_;
    std::deque<std::string> entries_;
    size_t max_size_ = 10000;

public:
    void push(const std::string& line) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (entries_.size() >= max_size_) entries_.pop_front();
        entries_.push_back(line);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return entries_.size();
    }

    // 获取末尾 count 条
    std::vector<std::string> get_tail(size_t count) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (entries_.empty()) return {};
        size_t start = (count >= entries_.size()) ? 0 : entries_.size() - count;
        return {entries_.begin() + static_cast<long>(start), entries_.end()};
    }

    // 获取 [start, end) 区间的条目
    std::vector<std::string> get_range(size_t start, size_t end) const {
        std::lock_guard<std::mutex> lk(mtx_);
        if (start >= entries_.size()) return {};
        if (end > entries_.size()) end = entries_.size();
        if (start >= end) return {};
        return {entries_.begin() + static_cast<long>(start),
                entries_.begin() + static_cast<long>(end)};
    }

    // 获取指定家族的日志 (过滤)
    std::vector<std::pair<size_t, std::string>>
    get_family_logs(const std::string& family_id, size_t max_count) const {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<std::pair<size_t, std::string>> result;
        std::string pattern = "家族" + family_id;
        for (size_t i = entries_.size(); i > 0 && result.size() < max_count; --i) {
            const auto& entry = entries_[i - 1];
            if (entry.find(pattern) != std::string::npos) {
                result.emplace_back(i - 1, entry);
            }
        }
        std::reverse(result.begin(), result.end());
        return result;
    }
};

extern LogQueue g_log_queue;

// ==================== 家族控制标志 ====================
struct FamilyControl {
    std::atomic<bool> paused{false};
    std::atomic<bool> force_stop{false};
    std::atomic<time_t> last_activity{0};
    std::atomic<int> pending_count{0};
};

extern std::unordered_map<std::string, std::shared_ptr<FamilyControl>> g_family_controls;

// ==================== 家族静态信息 ====================
struct TuiFamilyInfo {
    std::string family_id;
    std::string token_mask;
    std::string uid;
    bool post_enabled = false;
    bool comment_enabled = false;
    bool join_enabled = false;
    bool up_enabled = false;
    int min_level = -1;
    std::shared_ptr<FamilyControl> control;
};

extern std::vector<TuiFamilyInfo> g_family_list;

// ==================== 家族动态统计快照 ====================
struct TuiFamilyStats {
    int post_total = 0, post_approved = 0, post_rejected = 0;
    int comment_total = 0, comment_approved = 0, comment_rejected = 0;
    int join_total = 0, join_approved = 0, join_rejected = 0;
    int up_total = 0, up_approved = 0, up_rejected = 0;
};

// main.cc 设置此回调供 TUI 读取最新统计
extern std::function<TuiFamilyStats(const std::string& family_id)> g_get_family_stats;

// ==================== 全局运行标志 ====================
extern std::atomic<bool> g_running;

// ==================== 主题 ====================
enum class Theme { TOKYO_NIGHT, DARK, LIGHT };
extern Theme g_theme;

// ==================== TUI 入口 ====================
void run_tui();
