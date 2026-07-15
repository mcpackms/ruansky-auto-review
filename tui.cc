// tui.cc - TUI dashboard for auto_review
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

#include "tui.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <locale.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <ncurses.h>
#include <toml.hpp>

// ======================== 颜色对 ========================
enum ColorPair {
    CP_HEADER_BG = 1,  // fg on cardBg   - 顶栏背景
    CP_SECTION = 2,    // cyan on bg      - 区块标题
    CP_RUNNING = 3,    // green on bg     - 运行中
    CP_PAUSED = 4,     // red on bg       - 暂停/错误
    CP_SELECTED = 5,   // fg on selBg     - 选中行
    CP_FOOTER_BG = 6,  // gray on bg      - 底栏
    CP_CONFIG_KEY = 7,  // yellow on bg   - 配置项键名
    CP_CONFIG_VAL = 8,  // fg on bg       - 配置项值
    CP_SCROLL = 9,     // gray on bg      - 滚动提示
    CP_STATS = 10,     // purple on bg    - 统计数字
    CP_BORDER = 11,    // gray2 on bg     - 分隔/边框
};

// ======================== 辅助函数 ========================

static std::string truncate(const std::string& s, int max_w) {
    if (max_w <= 0) return "";
    if ((int)s.size() <= max_w) return s;
    return s.substr(0, std::max(0, max_w - 1)) + "…";
}

static std::string pad_right(const std::string& s, int w) {
    if ((int)s.size() >= w) return s.substr(0, w);
    return s + std::string(w - s.size(), ' ');
}

static std::string pad_left(const std::string& s, int w) {
    if ((int)s.size() >= w) return s.substr(0, w);
    return std::string(w - s.size(), ' ') + s;
}

static std::string fmt_stat(int total, int approved, int rejected) {
    return std::to_string(total) + "/" + std::to_string(approved) + "/" + std::to_string(rejected);
}

static std::string fmt_rate(int approved, int total) {
    if (total <= 0) return "  -";
    int pct = (int)std::round(approved * 100.0 / total);
    if (pct == 100) return "100";
    return (pct < 10 ? " " : "") + std::to_string(pct);
}

static std::string t_now() {
    time_t t = time(nullptr);
    struct tm lt_buf;
    struct tm* lt = localtime_r(&t, &lt_buf);
    char buf[9];
    snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
    return buf;
}

static std::string bool_str(bool v) {
    return v ? "开" : "关";
}

// ======================== TUI 状态 ========================
enum ViewMode { VIEW_MAIN = 0, VIEW_CONFIG = 1 };
static ViewMode g_mode = VIEW_MAIN;

// 主界面状态
static int g_selected = 0;
static int g_table_scroll = 0;
static int g_log_scroll = 0;
static int g_focus = 0; // 0=表格, 1=日志

// 配置编辑状态
static int g_cfg_selected = 0;
static int g_cfg_scroll = 0;
static bool g_config_dirty = false;

// ======================== 配置编辑数据 ========================
// 从 settings.toml 加载的编辑副本
static toml::value g_edit_toml;

// 可编辑字段描述
struct CfgField {
    enum Type { TOGGLE, NUMBER, SECTION } type;
    std::string label;      // 显示名称
    std::string key;        // TOML key
    int token_idx;          // -1=全局, 0..n=对应 TOKENS[]
    int min_val = 0, max_val = 0;   // 仅 NUMBER
};

static std::vector<CfgField> g_cfg_fields;

static void build_cfg_fields() {
    g_cfg_fields.clear();

    // 全局设置
    g_cfg_fields.push_back({CfgField::SECTION, "全局设置", "", -1});
    g_cfg_fields.push_back({CfgField::TOGGLE, "正则过滤", "ENABLE_REGEX", -1});
    g_cfg_fields.push_back({CfgField::NUMBER, "检查间隔(秒)", "CHECK_INTERVAL_SECONDS", -1, 1, 3600});
    g_cfg_fields.push_back({CfgField::NUMBER, "请求延迟(毫秒)", "REQUEST_DELAY_MS", -1, 0, 10000});
    g_cfg_fields.push_back({CfgField::NUMBER, "并发数", "CONCURRENCY", -1, 1, 100});
    g_cfg_fields.push_back({CfgField::NUMBER, "UP资源最大金币", "MAX_UP_RESOURCE_COIN", -1, -1, 999});

    // 每个 Token 的设置
    auto tokens_arr = toml::find_or(g_edit_toml, "TOKENS", toml::array());
    for (int i = 0; i < (int)tokens_arr.size(); ++i) {
        auto& tok = tokens_arr[i];
        std::string mask = "";
        try {
            std::string t = toml::find<std::string>(tok, "TOKEN");
            if (t.size() > 8) mask = t.substr(0, 4) + "****" + t.substr(t.size() - 4);
            else mask = std::string(t.size(), '*');
        } catch (...) { mask = "Token#" + std::to_string(i); }

        g_cfg_fields.push_back({CfgField::SECTION, "Token: " + mask, "", i});
        g_cfg_fields.push_back({CfgField::TOGGLE, "正则过滤(覆盖)", "ENABLE_REGEX", i});
        g_cfg_fields.push_back({CfgField::TOGGLE, "违禁词检查(覆盖)", "ENABLE_BADWORDS", i});
        g_cfg_fields.push_back({CfgField::NUMBER, "并发数(覆盖)", "CONCURRENCY", i, 1, 100});
        g_cfg_fields.push_back({CfgField::NUMBER, "延迟(覆盖)", "REQUEST_DELAY_MS", i, 0, 10000});
    }
}

// 读取字段值
static std::string cfg_field_value(const CfgField& f) {
    try {
        if (f.token_idx < 0) {
            // 全局
            switch (f.type) {
            case CfgField::TOGGLE:
                return bool_str(toml::find<bool>(g_edit_toml, f.key));
            case CfgField::NUMBER:
                return std::to_string(toml::find<int>(g_edit_toml, f.key));
            default: return "";
            }
        } else {
            // Token 覆盖
            auto& tokens2 = toml::find(g_edit_toml, "TOKENS");
            auto& tok = tokens2.at(f.token_idx);
            if (!tok.contains(f.key)) return "(使用全局)";
            switch (f.type) {
            case CfgField::TOGGLE:
                return bool_str(toml::find<bool>(tok, f.key));
            case CfgField::NUMBER:
                return std::to_string(toml::find<int>(tok, f.key));
            default: return "";
            }
        }
    } catch (...) {
        return "?";
    }
}

// 切换/修改字段值
static void cfg_field_toggle(const CfgField& f) {
    try {
        if (f.token_idx < 0) {
            bool v = toml::find<bool>(g_edit_toml, f.key);
            g_edit_toml[f.key] = !v;
        } else {
            auto& tokens3 = toml::find(g_edit_toml, "TOKENS");
            auto& tok = tokens3.at(f.token_idx);
            if (!tok.contains(f.key)) {
                // 添加字段，默认 true
                tok[f.key] = true;
            } else {
                bool v = toml::find<bool>(tok, f.key);
                tok[f.key] = !v;
            }
        }
        g_config_dirty = true;
    } catch (...) {}
}

static void cfg_field_adjust(const CfgField& f, int delta) {
    try {
        int v;
        if (f.token_idx < 0) {
            v = toml::find<int>(g_edit_toml, f.key);
            v = std::clamp(v + delta, f.min_val, f.max_val);
            g_edit_toml[f.key] = v;
        } else {
            auto& tokens4 = toml::find(g_edit_toml, "TOKENS");
            auto& tok = tokens4.at(f.token_idx);
            if (!tok.contains(f.key)) {
                v = std::clamp(delta > 0 ? f.min_val : f.max_val, f.min_val, f.max_val);
                tok[f.key] = v;
            } else {
                v = toml::find<int>(tok, f.key);
                v = std::clamp(v + delta, f.min_val, f.max_val);
                tok[f.key] = v;
            }
        }
        g_config_dirty = true;
    } catch (...) {}
}

static void load_edit_toml() {
    try {
        g_edit_toml = toml::parse("settings.toml");
    } catch (const std::exception& e) {
        g_edit_toml = toml::value();
    }
    build_cfg_fields();
}

static void save_edit_toml() {
    try {
        std::ofstream f("settings.toml");
        f << std::setw(120) << g_edit_toml;
        f.close();
        g_config_dirty = false;
    } catch (const std::exception& e) {
        // log error
    }
}

// ======================== 交互操作 ========================

static void toggle_pause(int idx) {
    if (idx < 0 || idx >= (int)g_family_list.size()) return;
    auto& info = g_family_list[idx];
    if (!info.control) return;
    bool new_state = !info.control->paused.load();
    info.control->paused.store(new_state);
    std::string action = new_state ? "⏸ 暂停" : "▶ 恢复";
    g_log_queue.push("[" + t_now() + "] [TUI] " + action + "\t家族" + info.family_id);
}

// ======================== 主界面渲染 ========================

static void draw_header(int cols) {
    // 蓝色背景顶栏
    wattron(stdscr, COLOR_PAIR(CP_HEADER_BG));
    mvwhline(stdscr, 0, 0, ' ', cols);

    wattron(stdscr, A_BOLD);
    mvwprintw(stdscr, 0, 2, "high_bot 自动审核 TUI  v1.3.0");
    wattroff(stdscr, A_BOLD);

    std::string hints;
    if (g_mode == VIEW_MAIN) {
        hints = " Q退出  P暂停  C配置  Tab切换焦点";
    } else {
        hints = g_config_dirty ? " Q返回  S保存*  E编辑器  Space切换  +/-调整" :
                                 " Q返回  S保存  E编辑器  Space切换  +/-调整";
    }
    mvwprintw(stdscr, 0, cols - (int)hints.size() - 2, "%s", hints.c_str());
    wattroff(stdscr, COLOR_PAIR(CP_HEADER_BG));
}

static void draw_separator(int y, int cols) {
    wattron(stdscr, COLOR_PAIR(CP_BORDER));
    mvwhline(stdscr, y, 0, ACS_HLINE, cols);
    wattroff(stdscr, COLOR_PAIR(CP_BORDER));
}

static void draw_section_title(int y, const std::string& icon, const std::string& title) {
    wattron(stdscr, A_BOLD | COLOR_PAIR(CP_SECTION));
    mvwprintw(stdscr, y, 2, "%s %s", icon.c_str(), title.c_str());
    wattroff(stdscr, A_BOLD | COLOR_PAIR(CP_SECTION));
}

// ---------- 家族总览表格 ----------
static void draw_family_table(int y0, int h, int cols) {
    int n = (int)g_family_list.size();
    if (n == 0) { mvwprintw(stdscr, y0, 2, "暂无家族数据"); return; }

    // 标题
    int online = 0;
    for (auto& info : g_family_list)
        if (info.control && !info.control->paused.load()) online++;
    draw_section_title(y0, "📋", "家族总览  (" + std::to_string(online) + "/" + std::to_string(n) + " 在线)");

    // 表头
    int hdr_y = y0 + 1;
    int cx = 2;
    int w_id = 5, w_tok = 12, w_st = 9;
    wattron(stdscr, A_BOLD);
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("ID", w_id).c_str()); cx += w_id;
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("Token", w_tok).c_str()); cx += w_tok;
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("帖", w_st).c_str()); cx += w_st;
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("评", w_st).c_str()); cx += w_st;
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("入", w_st).c_str()); cx += w_st;
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("UP", w_st).c_str()); cx += w_st;
    mvwprintw(stdscr, hdr_y, cx,    "%s", pad_right("资", w_st).c_str()); cx += w_st;
    mvwprintw(stdscr, hdr_y, cx,    "状态");
    wattroff(stdscr, A_BOLD);

    // 数据行
    int data_rows = h - 2;
    if (data_rows <= 0) return;

    if (g_selected < g_table_scroll) g_table_scroll = g_selected;
    if (g_selected >= g_table_scroll + data_rows) g_table_scroll = g_selected - data_rows + 1;
    g_table_scroll = std::max(0, std::min(g_table_scroll, std::max(0, n - data_rows)));

    int max_disp = std::min(data_rows, n - g_table_scroll);
    for (int i = 0; i < max_disp; ++i) {
        int idx = g_table_scroll + i;
        auto& info = g_family_list[idx];
        auto st = g_get_family_stats ? g_get_family_stats(info.family_id) : TuiFamilyStats();
        int row = hdr_y + 1 + i;
        bool sel = (idx == g_selected);
        if (sel) wattron(stdscr, COLOR_PAIR(CP_SELECTED));

        cx = 2;
        mvwprintw(stdscr, row, cx, "%s", pad_right(info.family_id, w_id).c_str()); cx += w_id;
        mvwprintw(stdscr, row, cx, "%s", pad_right(info.token_mask, w_tok).c_str()); cx += w_tok;
        mvwprintw(stdscr, row, cx, "%s", pad_right(fmt_stat(st.post_total, st.post_approved, st.post_rejected), w_st).c_str()); cx += w_st;
        mvwprintw(stdscr, row, cx, "%s", pad_right(fmt_stat(st.comment_total, st.comment_approved, st.comment_rejected), w_st).c_str()); cx += w_st;
        mvwprintw(stdscr, row, cx, "%s", pad_right(fmt_stat(st.join_total, st.join_approved, st.join_rejected), w_st).c_str()); cx += w_st;
        mvwprintw(stdscr, row, cx, "%s", pad_right(fmt_stat(st.up_total, st.up_approved, st.up_rejected), w_st).c_str()); cx += w_st;
        mvwprintw(stdscr, row, cx, "%s", pad_right(fmt_stat(st.up_resource_total, st.up_resource_approved, st.up_resource_rejected), w_st).c_str()); cx += w_st;

        bool paused = info.control && info.control->paused.load();
        if (paused) {
            wattron(stdscr, COLOR_PAIR(CP_PAUSED));
            mvwprintw(stdscr, row, cx, "⏸暂停");
            wattroff(stdscr, COLOR_PAIR(CP_PAUSED));
        } else {
            wattron(stdscr, COLOR_PAIR(CP_RUNNING));
            mvwprintw(stdscr, row, cx, "▶运行");
            wattroff(stdscr, COLOR_PAIR(CP_RUNNING));
        }
        if (sel) wattroff(stdscr, COLOR_PAIR(CP_SELECTED));
    }

    // 滚动提示
    if (g_table_scroll > 0) {
        wattron(stdscr, COLOR_PAIR(CP_SCROLL));
        mvwprintw(stdscr, hdr_y, cols - 8, "↑%d", g_table_scroll);
        wattroff(stdscr, COLOR_PAIR(CP_SCROLL));
    }
    if (g_table_scroll + data_rows < n) {
        wattron(stdscr, COLOR_PAIR(CP_SCROLL));
        mvwprintw(stdscr, hdr_y + data_rows, cols - 8, "↓%d", n - g_table_scroll - data_rows);
        wattroff(stdscr, COLOR_PAIR(CP_SCROLL));
    }
}

// ---------- 详情面板 ----------
static void draw_detail(int y0, int h, int cols) {
    if (g_selected < 0 || g_selected >= (int)g_family_list.size()) {
        mvwprintw(stdscr, y0, 2, "（未选中家族）");
        return;
    }
    auto& info = g_family_list[g_selected];
    auto st = g_get_family_stats ? g_get_family_stats(info.family_id) : TuiFamilyStats();
    bool paused = info.control && info.control->paused.load();
    time_t last = info.control ? info.control->last_activity.load() : 0;

    draw_section_title(y0, "🔍", "详情: 家族 #" + info.family_id);

    // 第二行: token / uid / 状态
    std::string status_str = paused ? "⏸ 已暂停" : "▶ 运行中";
    int st_color = paused ? CP_PAUSED : CP_RUNNING;

    mvwprintw(stdscr, y0 + 1, 2, "Token: %s  UID: %s",
              info.token_mask.c_str(), info.uid.c_str());
    wattron(stdscr, A_BOLD | COLOR_PAIR(st_color));
    mvwprintw(stdscr, y0 + 1, cols / 2, " %s ", status_str.c_str());
    wattroff(stdscr, A_BOLD | COLOR_PAIR(st_color));

    if (h >= 3) {
        int y = y0 + 2;
        int qw = std::max(1, cols / 5);
        auto draw_mod = [&](int col, const std::string& name, int t, int a, int r) {
            std::string s = name + " " + fmt_stat(t, a, r);
            std::string rate = fmt_rate(a, t);
            mvwprintw(stdscr, y, col, "%s", s.c_str());
            if (t > 0) {
                wattron(stdscr, COLOR_PAIR(CP_STATS));
                mvwprintw(stdscr, y, col + (int)s.size() + 1, "%s%%", rate.c_str());
                wattroff(stdscr, COLOR_PAIR(CP_STATS));
            }
        };
        draw_mod(2,  "帖", st.post_total, st.post_approved, st.post_rejected);
        draw_mod(qw, "评", st.comment_total, st.comment_approved, st.comment_rejected);
        draw_mod(qw*2, "入", st.join_total, st.join_approved, st.join_rejected);
        draw_mod(qw*3, "UP", st.up_total, st.up_approved, st.up_rejected);
        draw_mod(qw*4, "资", st.up_resource_total, st.up_resource_approved, st.up_resource_rejected);
    }

    // 最后活动
    if (h >= 4) {
        std::string last_str = "从未";
        if (last > 0) {
            struct tm lt_buf2;
            struct tm* lt = localtime_r(&last, &lt_buf2);
            char buf[9];
            snprintf(buf, sizeof(buf), "%02d:%02d:%02d", lt->tm_hour, lt->tm_min, lt->tm_sec);
            last_str = buf;
        }
        mvwprintw(stdscr, y0 + 3, 2, "模块: %s%s%s%s%s  最后活动: %s",
                  info.post_enabled ? "帖 " : "",
                  info.comment_enabled ? "评 " : "",
                  info.join_enabled ? "入 " : "",
                  info.up_enabled ? "UP " : "",
                  info.up_resource_enabled ? "资" : "",
                  last_str.c_str());
    }

    // 第5行: UP资源最大金币
    if (h >= 5 && info.up_resource_enabled) {
        std::string coin_str = info.max_up_resource_coin >= 0
            ? std::to_string(info.max_up_resource_coin)
            : "∞";
        mvwprintw(stdscr, y0 + 4, 2, "UP资源最大金币: %s", coin_str.c_str());
    }
}

// ---------- 日志面板 ----------
static void draw_log(int y0, int h, int cols) {
    std::string fam_id;
    if (g_selected >= 0 && g_selected < (int)g_family_list.size())
        fam_id = g_family_list[g_selected].family_id;
    std::string title = fam_id.empty() ? "日志" : ("日志 (家族 #" + fam_id + ")");

    if (g_focus == 1) {
        wattron(stdscr, A_BOLD | COLOR_PAIR(CP_SELECTED));
        mvwprintw(stdscr, y0, 2, "📜 %s", title.c_str());
        wattroff(stdscr, A_BOLD | COLOR_PAIR(CP_SELECTED));
    } else {
        draw_section_title(y0, "📜", title);
    }

    int data_rows = h - 1;
    if (data_rows <= 0) return;

    auto filtered = g_log_queue.get_family_logs(fam_id, 10000);
    size_t n = filtered.size();

    g_log_scroll = std::max(0, std::min(g_log_scroll, (int)std::max((size_t)0, n - 1)));
    int disp_start;
    if (g_log_scroll == 0) {
        disp_start = std::max(0, (int)n - data_rows);
    } else {
        disp_start = std::max(0, (int)n - g_log_scroll - data_rows);
    }

    int disp_cnt = std::min(data_rows, (int)n - disp_start);
    for (int i = 0; i < disp_cnt; ++i) {
        int idx = disp_start + i;
        if (idx < 0 || idx >= (int)n) break;
        std::string msg = filtered[idx].second;

        // Strip [INFO]/[ERROR] prefix
        std::string display = msg;
        if (display.size() > 7 && display[0] == '[') {
            auto p = display.find(']');
            if (p != std::string::npos) display = display.substr(p + 2);
        }

        bool is_err = (msg.find("[ERROR]") != std::string::npos);
        int row = y0 + 1 + i;
        if (is_err) wattron(stdscr, COLOR_PAIR(CP_PAUSED));
        mvwprintw(stdscr, row, 2, "%s", truncate(display, cols - 4).c_str());
        if (is_err) wattroff(stdscr, COLOR_PAIR(CP_PAUSED));
    }

    // 清空剩余行
    for (int i = disp_cnt; i < data_rows; ++i)
        mvwprintw(stdscr, y0 + 1 + i, 2, "%s", std::string(cols - 4, ' ').c_str());

    // 滚动提示
    if (g_log_scroll > 0 && n > 0) {
        wattron(stdscr, COLOR_PAIR(CP_SCROLL));
        mvwprintw(stdscr, y0 + data_rows, cols - 8, "↑%d", g_log_scroll);
        wattroff(stdscr, COLOR_PAIR(CP_SCROLL));
    } else if (g_log_scroll == 0 && n > (size_t)data_rows && g_focus == 1) {
        wattron(stdscr, COLOR_PAIR(CP_SCROLL));
        mvwprintw(stdscr, y0 + data_rows, cols - 12, "↑查看历史");
        wattroff(stdscr, COLOR_PAIR(CP_SCROLL));
    }
}

// ======================== 配置界面渲染 ========================

static void draw_config_view(int rows, int cols) {
    int y = 2; // 从标题下方开始

    for (int i = 0; i < (int)g_cfg_fields.size(); ++i) {
        if (y >= rows - 2) {
            // 显示剩余行数提示
            wattron(stdscr, COLOR_PAIR(CP_SCROLL));
            mvwprintw(stdscr, rows - 2, 2, "… 还有 %d 项", (int)g_cfg_fields.size() - i);
            wattroff(stdscr, COLOR_PAIR(CP_SCROLL));
            break;
        }

        auto& f = g_cfg_fields[i];
        bool sel = (i == g_cfg_selected);

        switch (f.type) {
        case CfgField::SECTION: {
            if (sel) wattron(stdscr, COLOR_PAIR(CP_SELECTED));
            wattron(stdscr, A_BOLD | COLOR_PAIR(CP_SECTION));
            mvwprintw(stdscr, y, 2, "── %s ──", f.label.c_str());
            wattroff(stdscr, A_BOLD | COLOR_PAIR(CP_SECTION));
            if (sel) wattroff(stdscr, COLOR_PAIR(CP_SELECTED));
            y++;
            break;
        }
        case CfgField::TOGGLE: {
            if (sel) wattron(stdscr, COLOR_PAIR(CP_SELECTED));
            std::string val = cfg_field_value(f);
            std::string disp = "  " + pad_right(f.label, 24) + pad_right(f.key, 24);
            mvwprintw(stdscr, y, 2, "%s", disp.c_str());
            if (val == "开") {
                wattron(stdscr, COLOR_PAIR(CP_RUNNING));
                mvwprintw(stdscr, y, 2 + (int)disp.size(), "[✔]");
                wattroff(stdscr, COLOR_PAIR(CP_RUNNING));
            } else {
                wattron(stdscr, COLOR_PAIR(CP_PAUSED));
                mvwprintw(stdscr, y, 2 + (int)disp.size(), "[ ]");
                wattroff(stdscr, COLOR_PAIR(CP_PAUSED));
            }
            mvwprintw(stdscr, y, 2 + (int)disp.size() + 4, "%s", val.c_str());
            if (sel) wattroff(stdscr, COLOR_PAIR(CP_SELECTED));
            y++;
            break;
        }
        case CfgField::NUMBER: {
            if (sel) wattron(stdscr, COLOR_PAIR(CP_SELECTED));
            std::string val = cfg_field_value(f);
            std::string disp = "  " + pad_right(f.label, 24) + pad_right(f.key, 24);
            mvwprintw(stdscr, y, 2, "%s", disp.c_str());
            std::string rng = "[" + std::to_string(f.min_val) + "~" + std::to_string(f.max_val) + "]";
            mvwprintw(stdscr, y, cols - (int)rng.size() - 20, "%s", rng.c_str());
            mvwprintw(stdscr, y, cols - 18, "%s", pad_left(val, 6).c_str());
            if (sel) wattroff(stdscr, COLOR_PAIR(CP_SELECTED));
            y++;
            break;
        }
        }
    }
}

// ======================== 输入处理 ========================

static void handle_main_input(int ch, int rows) {
    switch (ch) {
    case 'q': case 'Q': g_running = false; break;
    case 'c': case 'C':
        load_edit_toml();
        g_mode = VIEW_CONFIG;
        g_cfg_selected = 0;
        g_cfg_scroll = 0;
        g_config_dirty = false;
        break;
    case 'p': case 'P': toggle_pause(g_selected); break;
    case 9: // Tab
        g_focus = (g_focus == 0) ? 1 : 0;
        break;
    case KEY_UP:
        if (g_focus == 0) { if (g_selected > 0) g_selected--; }
        else { g_log_scroll++; }
        break;
    case KEY_DOWN:
        if (g_focus == 0) { if (g_selected < (int)g_family_list.size()-1) g_selected++; }
        else { if (g_log_scroll > 0) g_log_scroll--; }
        break;
    case KEY_PPAGE:
        if (g_focus == 0) g_selected = std::max(0, g_selected - rows/4);
        else g_log_scroll += rows/2;
        break;
    case KEY_NPAGE:
        if (g_focus == 0) g_selected = std::min((int)g_family_list.size()-1, g_selected + rows/4);
        else g_log_scroll = std::max(0, g_log_scroll - rows/2);
        break;
    case KEY_HOME:
        if (g_focus == 0) g_selected = 0; else g_log_scroll = 0;
        break;
    case KEY_END:
        if (g_focus == 0) g_selected = (int)g_family_list.size()-1; else g_log_scroll = 0;
        break;
    }
    // Clamp
    g_selected = std::clamp(g_selected, 0, std::max(0, (int)g_family_list.size()-1));
    g_log_scroll = std::max(0, g_log_scroll);
}

static void handle_config_input(int ch) {
    switch (ch) {
    case 'q': case 'Q':
        if (g_config_dirty) {
            // 提示未保存
            g_log_queue.push("[" + t_now() + "] [TUI] ⚠\t配置已修改但未保存，按 S 保存");
        }
        g_mode = VIEW_MAIN;
        break;
    case 's': case 'S':
        save_edit_toml();
        g_log_queue.push("[" + t_now() + "] [TUI] ✅\t配置已保存到 settings.toml");
        break;
    case 'e': case 'E': {
        // 外部编辑器
        endwin();
        const char* editor = getenv("EDITOR");
        if (!editor || !*editor) editor = "nano";
        std::string cmd = std::string(editor) + " settings.toml";
        int ret = system(cmd.c_str());
        (void)ret;
        // 重新加载
        refresh();
        load_edit_toml();
        g_log_queue.push("[" + t_now() + "] [TUI] ✅\t配置已重新加载");
        break;
    }
    case ' ': // Space toggle bool
        if (g_cfg_selected >= 0 && g_cfg_selected < (int)g_cfg_fields.size()) {
            auto& f = g_cfg_fields[g_cfg_selected];
            if (f.type == CfgField::TOGGLE) cfg_field_toggle(f);
        }
        break;
    case '+': case '=':
        if (g_cfg_selected >= 0 && g_cfg_selected < (int)g_cfg_fields.size()) {
            auto& f = g_cfg_fields[g_cfg_selected];
            if (f.type == CfgField::NUMBER) cfg_field_adjust(f, 1);
        }
        break;
    case '-': case '_':
        if (g_cfg_selected >= 0 && g_cfg_selected < (int)g_cfg_fields.size()) {
            auto& f = g_cfg_fields[g_cfg_selected];
            if (f.type == CfgField::NUMBER) cfg_field_adjust(f, -1);
        }
        break;
    case KEY_UP:
    case KEY_LEFT:
        if (g_cfg_selected > 0) g_cfg_selected--;
        break;
    case KEY_DOWN:
    case KEY_RIGHT:
        if (g_cfg_selected < (int)g_cfg_fields.size() - 1) g_cfg_selected++;
        break;
    case KEY_PPAGE:
        g_cfg_selected = std::max(0, g_cfg_selected - 10);
        break;
    case KEY_NPAGE:
        g_cfg_selected = std::min((int)g_cfg_fields.size() - 1, g_cfg_selected + 10);
        break;
    case KEY_HOME:
        g_cfg_selected = 0;
        break;
    case KEY_END:
        g_cfg_selected = (int)g_cfg_fields.size() - 1;
        break;
    case 9: // Tab - jump between sections
        // Find next section header
        for (int i = g_cfg_selected + 1; i < (int)g_cfg_fields.size(); ++i) {
            if (g_cfg_fields[i].type == CfgField::SECTION) {
                g_cfg_selected = i + 1;
                if (g_cfg_selected >= (int)g_cfg_fields.size())
                    g_cfg_selected = 0;
                break;
            }
        }
        break;
    }
    // Clamp
    g_cfg_selected = std::clamp(g_cfg_selected, 0, std::max(0, (int)g_cfg_fields.size() - 1));
}

// ======================== TUI 入口 ========================
void run_tui() {
    // ---- ncurses 初始化 ----
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);

    if (has_colors()) {
        start_color();
        use_default_colors();

        if (g_theme == Theme::TOKYO_NIGHT) {
            // Tokyo Night 真实配色
            // 参考: https://github.com/enkia/tokyo-night-vscode-theme
            if (can_change_color()) {
                // 自定义 256 色 (16-29)
                init_color(16, 102, 106, 149);   // bg      #1A1B26
                init_color(17, 753, 792, 961);   // fg      #C0CAF5
                init_color(18, 478, 635, 969);   // blue    #7AA2F7
                init_color(19, 490, 812, 1000);  // cyan    #7DCFFF
                init_color(20, 620, 808, 416);   // green   #9ECE6A
                init_color(21, 969, 463, 557);   // red     #F7768E
                init_color(22, 878, 686, 408);   // yellow  #E0AF68
                init_color(23, 733, 604, 969);   // purple  #BB9AF7
                init_color(24, 337, 373, 537);   // gray    #565F89
                init_color(25, 255, 282, 408);   // gray2   #414868
                init_color(26, 184, 231, 329);   // selBg   #2F3B54
                init_color(27, 231, 259, 380);   // gray3   #3B4261
                init_color(28, 122, 137, 208);   // cardBg  #1F2335

                // 所有背景 = bg (#1A1B26)
                int bg    = 16;
                int fg    = 17;
                int cyan  = 19;
                int green = 20;
                int red   = 21;
                int yellow= 22;
                int purple= 23;
                int gray  = 24;
                int gray2 = 25;
                int selBg = 26;
                int cardBg= 28;

                init_pair(CP_HEADER_BG,  fg, cardBg);   // fg on cardBg
                init_pair(CP_SECTION,    cyan,  bg);    // cyan on bg
                init_pair(CP_RUNNING,    green, bg);    // green on bg
                init_pair(CP_PAUSED,     red,   bg);    // red on bg
                init_pair(CP_SELECTED,   fg,    selBg); // fg on selBg
                init_pair(CP_FOOTER_BG,  fg,    cardBg);// fg on cardBg
                init_pair(CP_CONFIG_KEY, yellow,bg);    // yellow on bg
                init_pair(CP_CONFIG_VAL, fg,    bg);    // fg on bg
                init_pair(CP_SCROLL,     gray,  bg);    // gray on bg
                init_pair(CP_STATS,      purple,bg);    // purple on bg
                init_pair(CP_BORDER,     gray2, bg);    // gray2 on bg

                // 设置屏幕背景色 = bg
                bkgd(COLOR_PAIR(CP_SECTION) | ' ');
            } else {
                // fallback: 标准色 (黑底)
                init_pair(CP_HEADER_BG,  COLOR_WHITE, COLOR_BLACK);
                init_pair(CP_SECTION,    COLOR_CYAN,  COLOR_BLACK);
                init_pair(CP_RUNNING,    COLOR_GREEN, COLOR_BLACK);
                init_pair(CP_PAUSED,     COLOR_RED,   COLOR_BLACK);
                init_pair(CP_SELECTED,   COLOR_WHITE, COLOR_BLACK);
                init_pair(CP_FOOTER_BG,  COLOR_WHITE, COLOR_BLACK);
                init_pair(CP_CONFIG_KEY, COLOR_YELLOW,COLOR_BLACK);
                init_pair(CP_CONFIG_VAL, COLOR_WHITE, COLOR_BLACK);
                init_pair(CP_SCROLL,     8,           COLOR_BLACK);
                init_pair(CP_STATS,      COLOR_MAGENTA,COLOR_BLACK);
                init_pair(CP_BORDER,     COLOR_CYAN,  COLOR_BLACK);
                bkgd(COLOR_PAIR(CP_STATS) | ' ');
            }
        } else if (g_theme == Theme::DARK) {
            // dark: 黄白主色调，白底黑字选中
            init_pair(CP_HEADER_BG,   COLOR_WHITE,   -1);
            init_pair(CP_SECTION,     COLOR_YELLOW,  -1);
            init_pair(CP_RUNNING,     COLOR_GREEN,   -1);
            init_pair(CP_PAUSED,      COLOR_RED,     -1);
            init_pair(CP_SELECTED,    COLOR_BLACK,   COLOR_WHITE);
            init_pair(CP_FOOTER_BG,   -1,            -1);
            init_pair(CP_CONFIG_KEY,  COLOR_YELLOW,  -1);
            init_pair(CP_CONFIG_VAL,  COLOR_WHITE,   -1);
            init_pair(CP_SCROLL,      8,             -1);
            init_pair(CP_STATS,       COLOR_YELLOW,  -1);
        } else {
            // light: 深色文字配浅色背景
            init_pair(CP_HEADER_BG,   COLOR_BLACK,   COLOR_WHITE);
            init_pair(CP_SECTION,     COLOR_BLUE,    -1);
            init_pair(CP_RUNNING,     COLOR_GREEN,   -1);
            init_pair(CP_PAUSED,      COLOR_RED,     -1);
            init_pair(CP_SELECTED,    COLOR_WHITE,   COLOR_BLUE);
            init_pair(CP_FOOTER_BG,   COLOR_WHITE,   COLOR_BLUE);
            init_pair(CP_CONFIG_KEY,  COLOR_MAGENTA, -1);
            init_pair(CP_CONFIG_VAL,  COLOR_BLACK,   -1);
            init_pair(CP_SCROLL,      8,             -1);
            init_pair(CP_STATS,       COLOR_BLUE,    -1);
        }
    }

    // 初始加载配置
    load_edit_toml();

    // ---- 主循环 (10fps) ----
    while (g_running) {
        int rows, cols;
        getmaxyx(stdscr, rows, cols);

        if (rows < 12 || cols < 60) {
            werase(stdscr);
            std::string msg = "终端太小，请放大到 60x12 以上";
            mvwprintw(stdscr, rows/2, (cols-(int)msg.size())/2, "%s", msg.c_str());
            wrefresh(stdscr);
            napms(500);
            continue;
        }

        // ---- 输入 ----
        int ch = getch();
        if (g_mode == VIEW_MAIN) handle_main_input(ch, rows);
        else handle_config_input(ch);

        // ---- 清屏 ----
        werase(stdscr);

        // ---- 布局 ----
        int h_hdr = 1;
        int h_sep = 1;
        int h_ftr = 1;
        int rem = rows - h_hdr - h_ftr - 3 * h_sep;

        // ---- 渲染 ----
        draw_header(cols); // 行0

        if (g_mode == VIEW_MAIN) {
            int h_tbl = std::max(4, rem * 35 / 100);
            int h_det = std::max(3, rem * 18 / 100);
            int h_log = rem - h_tbl - h_det;

            draw_separator(1, cols);
            draw_family_table(2, h_tbl, cols);
            draw_separator(2 + h_tbl, cols);
            draw_detail(2 + h_tbl + 1, h_det, cols);
            draw_separator(2 + h_tbl + 1 + h_det, cols);
            draw_log(2 + h_tbl + 1 + h_det + 1, h_log, cols);
        } else {
            // 配置模式
            draw_separator(1, cols);
            draw_section_title(2, "⚙", "配置编辑");
            draw_config_view(rows, cols);
        }

        // 底部
        if (g_theme == Theme::TOKYO_NIGHT && has_colors()) {
            wattron(stdscr, COLOR_PAIR(CP_FOOTER_BG));
        } else {
            wattron(stdscr, A_REVERSE);
        }
        mvwhline(stdscr, rows - 1, 0, ' ', cols);
        if (g_mode == VIEW_MAIN) {
            std::string hint = g_focus == 0 ?
                " ↑↓ 选择家族  Tab 切换日志  P 暂停/恢复  C 配置  Q 退出" :
                " ↑↓ 滚动日志  Tab 切换列表  P 暂停/恢复  C 配置  Q 退出";
            mvwprintw(stdscr, rows - 1, 2, "%s", hint.c_str());
        } else {
            std::string hint = " ↑↓ 选择  Space 切换开关  +/- 调整数值  S 保存  E 外部编辑器  Q 返回";
            if (g_config_dirty)
                mvwprintw(stdscr, rows - 1, 2, "%s   ⚠ 有未保存的修改!", hint.c_str());
            else
                mvwprintw(stdscr, rows - 1, 2, "%s", hint.c_str());
        }
        if (g_theme == Theme::TOKYO_NIGHT && has_colors()) {
            wattroff(stdscr, COLOR_PAIR(CP_FOOTER_BG));
        } else {
            wattroff(stdscr, A_REVERSE);
        }

        wrefresh(stdscr);
        napms(100);
    }

    // ---- 清理 ----
    curs_set(1);
    endwin();
}
