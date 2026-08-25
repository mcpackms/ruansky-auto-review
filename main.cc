// main.cc - Automated content review bot
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

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <climits>
#include <cstdlib>

#include <toml.hpp>
#include <curl/curl.h>
#include <openssl/evp.h>
#include <nlohmann/json.hpp>
#include <functional>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "tui.h"
#include "web_panel.h"
#include "plugins/plugin_manager.h"
#include "version.h"

// ==================== 辅助函数 ====================
[[nodiscard]] static int safe_stoi(const std::string& s, int default_val = -1) {
    if (s.empty()) return default_val;
    char* end;
    long val = std::strtol(s.c_str(), &end, 10);
    if (end == s.c_str() || *end != '\0' || val < INT_MIN || val > INT_MAX) {
        return default_val;
    }
    return static_cast<int>(val);
}

[[nodiscard]] static std::string escape_log(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\033': out += "\\e"; break;
            default: out += c; break;
        }
    }
    return out;
}

// ==================== DAT + AC 自动机 (双数组 + Aho-Corasick) ====================
class DoubleArrayAC {
    // 临时 Trie (构建阶段使用)
    std::vector<std::vector<std::pair<unsigned char, int>>> trie_ch_;
    std::vector<std::vector<int>> trie_out_;
    std::vector<int> trie_dat_pos_;

    // DAT 核心数组
    std::vector<int> base_;
    std::vector<int> check_;
    std::vector<int> fail_;
    std::vector<std::vector<int>> out_;
    int next_candidate_ = 1;

    static constexpr int ROOT = 0;

    void dat_ensure(int pos) {
        if (pos >= (int)base_.size()) {
            int old = base_.size();
            int new_sz = std::max(pos + 1, old * 2);
            base_.resize(new_sz, 0);
            check_.resize(new_sz, -1);
            fail_.resize(new_sz, 0);
            out_.resize(new_sz);
        }
    }

    // DAT 单步转移: 返回下一位置, 无转移返回 -1
    [[nodiscard]] int go(int s, unsigned char c) const {
        if (s >= (int)base_.size()) return -1;
        int p = base_[s] + (int)c;
        if (p >= 0 && p < (int)check_.size() && check_[p] == s) return p;
        return -1;
    }

public:
    DoubleArrayAC() {
        trie_ch_.emplace_back();
        trie_out_.emplace_back();
        trie_dat_pos_.push_back(ROOT);
        dat_ensure(ROOT);
        base_[ROOT] = 1;
    }

    void insert(const std::string& word, int index) {
        int cur = 0;
        for (char c_ : word) {
            unsigned char c = static_cast<unsigned char>(c_);
            auto& children = trie_ch_[cur];
            auto it = std::find_if(children.begin(), children.end(),
                [c](const auto& p) { return p.first == c; });
            if (it != children.end()) {
                cur = it->second;
            } else {
                int nid = trie_ch_.size();
                trie_ch_.emplace_back();
                trie_out_.emplace_back();
                trie_dat_pos_.push_back(-1);
                trie_ch_[cur].emplace_back(c, nid);
                cur = nid;
            }
        }
        trie_out_[cur].push_back(index);
    }

    void build() {
        int n_trie = trie_ch_.size();
        if (n_trie == 0) return;

        // === 第1步: BFS 为每个节点分配 DAT 位置 (base + check) ===
        std::queue<int> q;
        q.push(0);

        while (!q.empty()) {
            int cur = q.front();
            q.pop();
            int cur_pos = trie_dat_pos_[cur];
            auto& children = trie_ch_[cur];
            if (children.empty()) continue;

            // 寻找合适的 base 值 (从游标开始，避免重复扫描已占位)
            int b = next_candidate_;
            while (true) {
                bool ok = true;
                for (auto& [cc, _] : children) {
                    int p = b + (int)cc;
                    if (p < (int)check_.size() && check_[p] != -1) {
                        ok = false;
                        break;
                    }
                }
                if (ok) break;
                b++;
            }
            next_candidate_ = b;

            base_[cur_pos] = b;

            for (auto& [cc, nid] : children) {
                int p = b + (int)cc;
                dat_ensure(p);
                check_[p] = cur_pos;
                trie_dat_pos_[nid] = p;
                q.push(nid);
            }
        }

        // === 第2步: 输出数据复制到 DAT ===
        for (int i = 0; i < n_trie; ++i) {
            int pos = trie_dat_pos_[i];
            out_[pos] = std::move(trie_out_[i]);
        }

        // === 第3步: BFS 构建 fail 链 ===
        std::queue<int> fq;

        for (auto& [_, nid] : trie_ch_[ROOT]) {
            int npos = trie_dat_pos_[nid];
            fail_[npos] = ROOT;
            out_[npos].insert(out_[npos].end(), out_[ROOT].begin(), out_[ROOT].end());
            fq.push(nid);
        }

        while (!fq.empty()) {
            int cur = fq.front();
            fq.pop();
            int cur_pos = trie_dat_pos_[cur];

            for (auto& [cc, nid] : trie_ch_[cur]) {
                int npos = trie_dat_pos_[nid];

                // 找 fail
                int f = fail_[cur_pos];
                while (f != ROOT && go(f, cc) < 0) {
                    f = fail_[f];
                }
                int fn = go(f, cc);
                fail_[npos] = fn >= 0 ? fn : ROOT;

                // 合并 fail 节点的输出
                int fail_pos = fail_[npos];
                out_[npos].insert(out_[npos].end(),
                    out_[fail_pos].begin(), out_[fail_pos].end());

                fq.push(nid);
            }
        }

        // 释放临时内存
        trie_ch_.clear();
        trie_ch_.shrink_to_fit();
        trie_out_.clear();
        trie_out_.shrink_to_fit();
        trie_dat_pos_.clear();
        trie_dat_pos_.shrink_to_fit();
    }

    // 搜索第一个匹配，命中即返回 (内联 go 避免函数调用和冗余边界检查)
    [[nodiscard]] int search_first(const std::string& text) const {
        int s = ROOT;
        for (char c_ : text) {
            unsigned char c = static_cast<unsigned char>(c_);
            // 内联 go() 循环 — 沿 fail 链找有效转移
            while (s != ROOT) {
                int p = base_[s] + (int)c;
                if (p < (int)check_.size() && check_[p] == s) break;
                s = fail_[s];
            }
            int p = base_[s] + (int)c;
            if (p < (int)check_.size() && check_[p] == s) s = p;
            if (!out_[s].empty()) return out_[s][0];
        }
        return -1;
    }
};

#include "config_types.h"

struct WordsData {
    std::vector<std::string> words;
    std::unique_ptr<DoubleArrayAC> ac_matcher;
    bool enabled = false;

    void build_ac_matcher() {
        if (!enabled || words.empty()) return;
        ac_matcher = std::make_unique<DoubleArrayAC>();
        for (size_t i = 0; i < words.size(); ++i) {
            ac_matcher->insert(words[i], static_cast<int>(i));
        }
        ac_matcher->build();
    }
};

struct Pcre2CodeDeleter {
    void operator()(pcre2_code* p) const noexcept { if (p) pcre2_code_free(p); }
};
using Pcre2CodePtr = std::unique_ptr<pcre2_code, Pcre2CodeDeleter>;

struct RegexData {
    std::vector<Pcre2CodePtr> patterns;
    bool enabled = false;
};

// ==================== 文本检查器 ====================
class TextChecker {
public:
    explicit TextChecker(const std::string& text)
        : original_(text), lower_cached_(false) {}

    [[nodiscard]] const std::string& to_lower() {
        if (!lower_cached_) {
            lower_.assign(original_);
            std::transform(lower_.begin(), lower_.end(), lower_.begin(), ::tolower);
            lower_cached_ = true;
        }
        return lower_;
    }

    [[nodiscard]] std::string_view original() const { return original_; }

private:
    const std::string& original_;
    std::string lower_;
    bool lower_cached_;
};

// ==================== 全局变量 ====================
std::atomic<bool> g_running{true};
Config g_config;
WordsData g_bad_words;
RegexData g_regex_data;
std::string g_config_path = "settings.toml";
std::atomic<int32_t> g_config_version{0};  // 递增后通知 family_loop 重载配置

// Web 面板 & 插件系统访问器（避免包含 DoubleArrayAC 定义）
int get_bad_words_count() { return static_cast<int>(g_bad_words.words.size()); }
bool get_bad_words_enabled() { return g_bad_words.enabled; }
int get_regex_patterns_count() { return static_cast<int>(g_regex_data.patterns.size()); }
bool get_regex_enabled() { return g_regex_data.enabled; }
using BadWordsList = std::vector<std::string>;
const BadWordsList& get_bad_words_list() { return g_bad_words.words; }

// ==================== 线程安全日志 ====================
class Logger {
    std::mutex mtx_;
    bool tui_mode_ = false;
    static constexpr size_t MAX_MSG_LEN = 2048;

public:
    void set_tui_mode(bool v) { tui_mode_ = v; }

    void info(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log("[INFO] ", fmt, args);
        va_end(args);
    }

    void error(const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log("[ERROR] ", fmt, args);
        va_end(args);
    }

private:
    void log(const char* level, const char* fmt, va_list args) {
        std::lock_guard<std::mutex> lk(mtx_);
        char buf[MAX_MSG_LEN];
        int pos = snprintf(buf, sizeof(buf), "%s", level);
        vsnprintf(buf + pos, sizeof(buf) - pos, fmt, args);
        std::string msg(buf);
        if (!tui_mode_) {
            printf("%s\n", msg.c_str());
            fflush(stdout);
        }
        g_log_queue.push(msg);
    }
} g_log;

// ==================== TUI 全局数据 ====================
LogQueue g_log_queue;
std::unordered_map<std::string, std::shared_ptr<FamilyControl>> g_family_controls;
std::vector<TuiFamilyInfo> g_family_list;
std::function<TuiFamilyStats(const std::string&)> g_get_family_stats;

// ==================== 一次运行模式 / 主题 ====================
bool g_once_mode = false;
bool g_no_tui = false;
int g_start_time = 0;
Theme g_theme = Theme::TOKYO_NIGHT;

// ==================== 统计 ====================
struct ReviewStats {
    std::atomic<int32_t> total{0}, approved{0}, rejected{0};
};

// 统计快照（无锁、无原子操作的一致副本）
struct ReviewStatsSnapshot {
    int32_t total = 0, approved = 0, rejected = 0;
};

class TokenReviewer {
public:
    std::string token;
    std::string uid;
    std::vector<FamilyConfig> families;

    // 返回一致快照（在锁保护下读取全部字段）
    ReviewStatsSnapshot get_post_stats_snapshot(const std::string& fid) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = post_stats_[fid];
        return {s.total.load(), s.approved.load(), s.rejected.load()};
    }
    ReviewStatsSnapshot get_comment_stats_snapshot(const std::string& fid) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = comment_stats_[fid];
        return {s.total.load(), s.approved.load(), s.rejected.load()};
    }
    ReviewStatsSnapshot get_join_stats_snapshot(const std::string& fid) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = join_stats_[fid];
        return {s.total.load(), s.approved.load(), s.rejected.load()};
    }
    ReviewStatsSnapshot get_up_stats_snapshot(const std::string& fid) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = up_stats_[fid];
        return {s.total.load(), s.approved.load(), s.rejected.load()};
    }
    ReviewStatsSnapshot get_up_resource_stats_snapshot(const std::string& fid) {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& s = up_resource_stats_[fid];
        return {s.total.load(), s.approved.load(), s.rejected.load()};
    }

    // 注意：以下 add_xx 方法在锁外做原子递增，单个 atomic 的 += 本身是原子的，
    // 但多个字段的跨字段一致性由读取方通过 get_xx_snapshot 保证。
    void add_post_total(const std::string& fid, int32_t d) { post_stats_[fid].total += d; }
    void add_post_approved(const std::string& fid, int32_t d) { post_stats_[fid].approved += d; }
    void add_post_rejected(const std::string& fid, int32_t d) { post_stats_[fid].rejected += d; }
    void add_comment_total(const std::string& fid, int32_t d) { comment_stats_[fid].total += d; }
    void add_comment_approved(const std::string& fid, int32_t d) { comment_stats_[fid].approved += d; }
    void add_comment_rejected(const std::string& fid, int32_t d) { comment_stats_[fid].rejected += d; }
    void add_join_total(const std::string& fid, int32_t d) { join_stats_[fid].total += d; }
    void add_join_approved(const std::string& fid, int32_t d) { join_stats_[fid].approved += d; }
    void add_join_rejected(const std::string& fid, int32_t d) { join_stats_[fid].rejected += d; }
    void add_up_total(const std::string& fid, int32_t d) { up_stats_[fid].total += d; }
    void add_up_approved(const std::string& fid, int32_t d) { up_stats_[fid].approved += d; }
    void add_up_rejected(const std::string& fid, int32_t d) { up_stats_[fid].rejected += d; }
    void add_up_resource_total(const std::string& fid, int32_t d) { up_resource_stats_[fid].total += d; }
    void add_up_resource_approved(const std::string& fid, int32_t d) { up_resource_stats_[fid].approved += d; }
    void add_up_resource_rejected(const std::string& fid, int32_t d) { up_resource_stats_[fid].rejected += d; }

private:
    std::mutex mtx_;
    std::unordered_map<std::string, ReviewStats> post_stats_;
    std::unordered_map<std::string, ReviewStats> comment_stats_;
    std::unordered_map<std::string, ReviewStats> join_stats_;
    std::unordered_map<std::string, ReviewStats> up_stats_;
    std::unordered_map<std::string, ReviewStats> up_resource_stats_;
};

struct PendingItem {
    std::string id;
    std::string family_id;
    std::string full_text;
    std::string token;
    std::string uid;
    std::string di;  // UP资源详情用的 di 参数
};

// ==================== RAII 封装 OpenSSL 对象 ====================
struct EvpMdCtxDeleter {
    void operator()(EVP_MD_CTX* p) const { if (p) EVP_MD_CTX_free(p); }
};
using EvpMdCtxPtr = std::unique_ptr<EVP_MD_CTX, EvpMdCtxDeleter>;

struct EvpCipherCtxDeleter {
    void operator()(EVP_CIPHER_CTX* p) const { if (p) EVP_CIPHER_CTX_free(p); }
};
using EvpCipherCtxPtr = std::unique_ptr<EVP_CIPHER_CTX, EvpCipherCtxDeleter>;

// ==================== CURL 句柄池 ====================
struct CurlHandle {
    CURL* easy;
    CurlHandle() {
        easy = curl_easy_init();
        curl_easy_setopt(easy, CURLOPT_TIMEOUT, 3L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 2L);
    }
    ~CurlHandle() {
        if (easy) curl_easy_cleanup(easy);
    }
    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

class CurlPool {
public:
    CurlHandle* acquire() {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!pool_.empty()) {
            auto* h = pool_.back();
            pool_.pop_back();
            curl_easy_reset(h->easy);
            curl_easy_setopt(h->easy, CURLOPT_TIMEOUT, 3L);
            curl_easy_setopt(h->easy, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(h->easy, CURLOPT_SSL_VERIFYHOST, 2L);
            return h;
        }
        return new CurlHandle();
    }

    void release(CurlHandle* h) {
        if (!h) return;
        std::lock_guard<std::mutex> lk(mtx_);
        if (pool_.size() < k_max_size_) {
            pool_.push_back(h);
        } else {
            delete h;
        }
    }

    ~CurlPool() {
        for (auto* h : pool_) delete h;
    }

private:
    std::mutex mtx_;
    std::vector<CurlHandle*> pool_;
    static constexpr size_t k_max_size_ = 10;
};

static thread_local CurlPool t_curl_pool;

struct CurlHandleDeleter {
    void operator()(CurlHandle* h) const {
        if (h) t_curl_pool.release(h);
    }
};
using CurlHandlePtr = std::unique_ptr<CurlHandle, CurlHandleDeleter>;

static CurlHandlePtr acquire_curl() {
    return CurlHandlePtr(t_curl_pool.acquire());
}

// ==================== 工具函数 ====================
[[nodiscard]] static std::string now_str() {
    time_t t = time(nullptr);
    struct tm tm_buf;
    char buf[20];
    strftime(buf, sizeof(buf), "%H:%M:%S", localtime_r(&t, &tm_buf));
    return buf;
}

[[nodiscard]] static std::string mask_token(const std::string& token) {
    if (token.size() <= 8) return std::string(token.size(), '*');
    return token.substr(0, 4) + "****" + token.substr(token.size() - 4);
}

[[nodiscard]] static std::string md5_hex(std::string_view s) {
    EvpMdCtxPtr ctx(EVP_MD_CTX_new());
    EVP_DigestInit_ex(ctx.get(), EVP_md5(), nullptr);
    EVP_DigestUpdate(ctx.get(), s.data(), s.size());
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    EVP_DigestFinal_ex(ctx.get(), digest, &len);
    char buf[33];
    for (unsigned int i = 0; i < len; ++i) {
        snprintf(buf + i * 2, 3, "%02x", digest[i]);
    }
    return buf;
}

[[nodiscard]] static std::string base64_decode(const std::string& in) {
    BIO* bio = BIO_new_mem_buf(in.data(), in.size());
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    bio = BIO_push(b64, bio);
    std::string out;
    out.resize(in.size());
    int len = BIO_read(bio, out.data(), out.size());
    BIO_free_all(bio);
    if (len <= 0) return "";
    out.resize(len);
    return out;
}

[[nodiscard]] static std::string decrypt_aes(const std::string& b64, const std::string& iv_b64 = "") {
    auto data = base64_decode(b64);
    if (data.size() % 16 != 0) throw std::runtime_error("bad block size");
    
    unsigned char key[16] = {0};
    memcpy(key, g_config.sign_const.data(), std::min<size_t>(g_config.sign_const.size(), 16));
    
    unsigned char iv[16] = {0};
    if (!iv_b64.empty()) {
        auto d = base64_decode(iv_b64);
        memcpy(iv, d.data(), std::min<size_t>(d.size(), 16));
    } else {
        memcpy(iv, key, 16);
    }

    EvpCipherCtxPtr ctx(EVP_CIPHER_CTX_new());
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_128_cbc(), nullptr, key, iv) != 1) {
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }
    
    // 禁用填充 - 与原逻辑保持一致
    EVP_CIPHER_CTX_set_padding(ctx.get(), 0);

    std::vector<unsigned char> out(data.size() + 16);
    int outlen = 0, tmplen = 0;
    if (EVP_DecryptUpdate(ctx.get(), out.data(), &outlen,
        (const unsigned char*)data.data(), data.size()) != 1) {
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }
    if (EVP_DecryptFinal_ex(ctx.get(), out.data() + outlen, &tmplen) != 1) {
        throw std::runtime_error("EVP_DecryptFinal_ex failed");
    }
    outlen += tmplen;
    out.resize(outlen);

    // 修复: 正确的 PKCS#7 填充处理
    if (!out.empty()) {
        uint8_t padding_byte = out.back();
        // PKCS#7 填充值范围 1 到 16
        if (padding_byte >= 1 && padding_byte <= 16 && padding_byte <= out.size()) {
            bool valid = true;
            for (size_t i = out.size() - padding_byte; i < out.size(); ++i) {
                if (out[i] != padding_byte) {
                    valid = false;
                    break;
                }
            }
            if (valid) {
                out.resize(out.size() - padding_byte);
            }
        }
    }
    
    return {out.begin(), out.end()};
}
[[nodiscard]] static std::string generate_sign(const std::unordered_map<std::string, std::string>& params,
    bool skip_empty_msg3 = false) {
    std::vector<std::string> entries;
    entries.reserve(params.size());
    for (auto& [k, v] : params) {
        if (k.find("$*$") == 0 || k == "key") continue;
        if (skip_empty_msg3 && k == "msg3" && v.empty()) continue;
        entries.push_back(k + "=" + v);
    }
    std::sort(entries.begin(), entries.end());
    std::string s = g_config.sign_const;
    if (!entries.empty()) {
        s += "&";
        for (size_t i = 0; i < entries.size(); ++i) {
            if (i) s += "&";
            s += entries[i];
        }
    }
    return md5_hex(s);
}

[[nodiscard]] static std::string url_encode(const std::string& s) {
    CURL* c = curl_easy_init();
    if (!c) return s;
    char* e = curl_easy_escape(c, s.c_str(), s.length());
    std::string r(e);
    curl_free(e);
    curl_easy_cleanup(c);
    return r;
}

[[nodiscard]] static std::string build_url(const std::string& ep,
    std::unordered_map<std::string, std::string>& params) {
    std::string b = g_config.base_url;
    if (b.back() == '/') b.pop_back();
    std::string u = b + ep + "?";
    bool first = true;
    if (auto it = params.find("family_id"); it != params.end()) {
        u += "family_id=" + url_encode(it->second);
        first = false;
    }
    if (auto it = params.find("mid"); it != params.end() && !it->second.empty()) {
        if (!first) u += "&";
        u += "mid=" + url_encode(it->second);
        first = false;
    }
    for (auto& [k, v] : params) {
        if (k == "key" || k == "family_id" || k == "mid") continue;
        if (!first) u += "&";
        first = false;
        std::string ok = (k.find("$*$") == 0) ? k.substr(3) : k;
        u += ok + "=" + url_encode(v);
    }
    if (auto it = params.find("key"); it != params.end()) {
        if (!first) u += "&";
        u += "key=" + url_encode(it->second);
    }
    return u;
}

[[nodiscard]] static std::string build_post_body(const std::unordered_map<std::string, std::string>& params,
    bool add_os) {
    std::string b;
    b.reserve(512);
    for (auto& [k, v] : params) {
        if (k != "key") b += k + "=" + v + "&";
    }
    if (add_os) b += "os_info=" + g_config.os_info + "&";
    b += "key=" + params.at("key") + "&";
    return b;
}

// ==================== 文本检查 (单次扫描) ====================
struct CheckResult {
    bool should_reject = false;
    std::string reason;
};

// 一次扫描完成违禁词 + 正则检查，避免 should_reject + reject_reason 双重扫描
[[nodiscard]] static CheckResult check_text(TextChecker& checker, bool ereg, bool ebad,
    const std::string& family_id = "", const std::string& type = "") {
    if (ereg && !g_regex_data.enabled) ereg = false;

    const std::string& lower = checker.to_lower();
    std::string orig_text(checker.original());

    // 插件 before-check 钩子
    if (!family_id.empty() && !type.empty()) {
        JsCheckResult plugin_result = g_plugin_mgr.dispatch_before_check(
            orig_text, family_id, type);
        if (plugin_result.should_reject) {
            return CheckResult{true, plugin_result.reason};
        }
    }

    CheckResult result;

    // 先查违禁词 (AC 自动机，非常快)
    if (ebad && g_bad_words.enabled && g_bad_words.ac_matcher) {
        int idx = g_bad_words.ac_matcher->search_first(lower);
        if (idx >= 0) {
            result.should_reject = true;
            if (idx < static_cast<int>(g_bad_words.words.size())) {
                result.reason = "违规";
            }
        }
    }

    // 再查正则 (PCRE2 相对较慢)
    if (!result.should_reject && ereg) {
        for (auto& re : g_regex_data.patterns) {
            pcre2_match_data* md = pcre2_match_data_create_from_pattern(re.get(), nullptr);
            int rc = pcre2_match(re.get(), (PCRE2_SPTR)lower.c_str(), lower.size(), 0, 0, md, nullptr);
            if (rc >= 0) {
                result.should_reject = true;
                PCRE2_SIZE* ovec = pcre2_get_ovector_pointer(md);
                PCRE2_SIZE start = ovec[0];
                PCRE2_SIZE end = ovec[1];
                std::string match;
                if (end > start) match = lower.substr(start, end - start);
                result.reason = "正则:" + (match.empty() ? "命中" : match);
                pcre2_match_data_free(md);
                break;
            }
            pcre2_match_data_free(md);
        }
    }

    // 插件 after-check 钩子
    if (!family_id.empty() && !type.empty()) {
        JsCheckResult plugin_result = g_plugin_mgr.dispatch_after_check(
            orig_text, JsCheckResult{result.should_reject, result.reason},
            family_id, type);
        if (plugin_result.should_reject) {
            result.should_reject = true;
            result.reason = plugin_result.reason;
        }
    }

    return result;
}

// ==================== HTTP 请求 ====================
static size_t write_cb(void* p, size_t s, size_t n, void* u) {
    size_t t = s * n;
    ((std::string*)u)->append((char*)p, t);
    return t;
}

[[nodiscard]] static std::string send_get(const std::string& ep,
    std::unordered_map<std::string, std::string>& params, CurlHandle& ch) {
    std::string url = build_url(ep, params);
    std::string resp;
    resp.reserve(4096);
    // 清除可能残留的 POST 选项
    curl_easy_setopt(ch.easy, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(ch.easy, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(ch.easy, CURLOPT_POSTFIELDSIZE, 0L);
    curl_easy_setopt(ch.easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(ch.easy, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(ch.easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(ch.easy, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(ch.easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(ch.easy, CURLOPT_USERAGENT, g_config.user_agent.c_str());
    CURLcode res = curl_easy_perform(ch.easy);
    if (res != CURLE_OK) throw std::runtime_error(curl_easy_strerror(res));
    return resp;
}

[[nodiscard]] static std::string send_post(const std::string& ep, const std::string& body,
    CurlHandle& ch) {
    std::string url = g_config.base_url;
    if (url.back() == '/') url.pop_back();
    url += ep;
    std::string resp;
    resp.reserve(4096);
    // 清除可能残留的 GET 选项
    curl_easy_setopt(ch.easy, CURLOPT_HTTPHEADER, NULL);
    curl_easy_setopt(ch.easy, CURLOPT_POSTFIELDS, NULL);
    curl_easy_setopt(ch.easy, CURLOPT_POSTFIELDSIZE, 0L);
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded; charset=UTF-8");
    curl_easy_setopt(ch.easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(ch.easy, CURLOPT_POST, 1L);
    curl_easy_setopt(ch.easy, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(ch.easy, CURLOPT_POSTFIELDSIZE, body.size());
    curl_easy_setopt(ch.easy, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(ch.easy, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(ch.easy, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(ch.easy, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(ch.easy, CURLOPT_USERAGENT, g_config.user_agent.c_str());
    CURLcode res = curl_easy_perform(ch.easy);
    curl_slist_free_all(headers);
    if (res != CURLE_OK) throw std::runtime_error(curl_easy_strerror(res));
    return resp;
}

// ==================== 业务逻辑 ====================
[[nodiscard]] static std::vector<std::unique_ptr<PendingItem>> get_pending_items(
    const ModuleConfig& mod, bool is_comment, const FamilyConfig& fam,
    const std::string& token, const std::string& uid, CurlHandle& ch) {
    std::unordered_map<std::string, std::string> params;
    params["family_id"] = fam.family_id;
    params["channel"] = g_config.channel;
    params["version"] = g_config.version;
    params["api_level"] = g_config.api_level;
    params["phone_model"] = g_config.phone_model;
    params["token"] = token;
    params[mod.use_status3 ? "status3" : "state3"] = mod.state3_pending;
    params["uid"] = uid;
    params["$*$page"] = g_config.page;
    params["$*$limit"] = g_config.limit;
    params["$*$os_info"] = g_config.os_info;
    if (!is_comment && !mod.use_status3) params["mid"] = fam.mid;
    params["key"] = generate_sign(params, false);

    auto resp = nlohmann::json::parse(send_get(mod.list_endpoint, params, ch));
    std::string dec;
    if (is_comment) {
        std::string d = resp.value("data", ""), iv = resp.value("iv", "");
        if (d.empty() || iv.empty()) return {};
        dec = decrypt_aes(d, iv);
    } else if (mod.use_status3) {
        // UP资源: 响应可能有 code, data 可能是加密字符串或直接 JSON 数组
        if (resp.contains("code")) {
            int c = resp["code"].is_number() ? resp["code"].get<int>()
                : (resp["code"].is_string() ? safe_stoi(resp["code"].get<std::string>()) : -1);
            if (c != 0) return {};
        }
        if (resp.contains("data") && resp["data"].is_string()) {
            std::string d = resp["data"].get<std::string>();
            if (d.empty()) return {};
            dec = decrypt_aes(d);
        } else if (resp.contains("data") && resp["data"].is_array()) {
            // data 已是明文数组，直接包装成解密后的格式
            nlohmann::json wrapper;
            wrapper["data"] = resp["data"];
            dec = wrapper.dump();
        } else {
            return {};
        }
    } else {
        if (!resp.value("validation", false)) return {};
        std::string d = resp.value("data", "");
        if (d.empty()) return {};
        dec = decrypt_aes(d);
    }

    auto result = nlohmann::json::parse(dec);
    if (!mod.use_status3 && is_comment && result.contains("code")) {
        int c = result["code"].is_number() ? result["code"].get<int>()
            : (result["code"].is_string() ? safe_stoi(result["code"].get<std::string>()) : -1);
        if (c != 0) return {};
    }

    if (!result.contains("data") || !result["data"].is_array() || result["data"].empty()) {
        return {};
    }

    std::vector<std::unique_ptr<PendingItem>> items;
    items.reserve(result["data"].size());
    std::vector<std::string> id_fields = is_comment
        ? std::vector<std::string>{"id", "cid"}
        : std::vector<std::string>{"id", "pid", "post_id"};

    auto id_field_str = [](const nlohmann::json& item, const std::vector<std::string>& fields) -> std::string {
        for (auto& field : fields) {
            if (!item.contains(field)) continue;
            if (item[field].is_string()) return item[field].get<std::string>();
            if (item[field].is_number_integer()) return std::to_string(item[field].get<int64_t>());
            if (item[field].is_number()) return std::to_string(item[field].get<double>());
        }
        return "";
    };
    std::vector<std::vector<std::string>> text_groups = is_comment
        ? std::vector<std::vector<std::string>>{{"content", "body"}}
        : std::vector<std::vector<std::string>>{{"content", "body"}, {"title", "subject"}};

    for (auto& item : result["data"]) {
        std::string id = id_field_str(item, id_fields);
        if (id.empty()) continue;

        std::string full_text;
        full_text.reserve(256);
        for (auto& group : text_groups) {
            for (auto& field : group) {
                if (!item.contains(field)) continue;
                if (item[field].is_string()) full_text += item[field].get<std::string>();
                else if (item[field].is_number_integer()) full_text += std::to_string(item[field].get<int64_t>());
                else if (item[field].is_number()) full_text += std::to_string(item[field].get<double>());
                full_text += " ";
                break;
            }
        }
        // UP资源需要提取 di 字段
        std::string di;
        if (mod.use_status3) {
            if (item.contains("di") && item["di"].is_string()) {
                di = item["di"].get<std::string>();
            }
        }

        items.emplace_back(std::make_unique<PendingItem>(
            PendingItem{id, fam.family_id, full_text, token, uid, di}));
    }
    return items;
}

[[nodiscard]] static bool approve_item(const ModuleConfig& mod, const std::string& item_id,
    const std::string& family_id, const std::string& token, const std::string& uid,
    const std::string& state3, bool is_comment, CurlHandle& ch) {
    std::unordered_map<std::string, std::string> base;
    base["uid"] = uid;
    base["api_level"] = g_config.api_level;
    base["family_id"] = family_id;
    base["channel"] = g_config.channel;
    base["version"] = g_config.version;
    base["phone_model"] = g_config.phone_model;
    base["token"] = token;
    base["state3"] = state3;
    base["msg3"] = "";
    if (is_comment) {
        base["os_info"] = g_config.os_info;
        base["cid"] = item_id;
    } else {
        base["pid"] = item_id;
    }

    std::unordered_map<std::string, std::string> sign_params;
    for (auto& [k, v] : base) {
        if (k == "msg3") continue;
        sign_params[(k == "os_info" ? "$*$os_info" : k)] = v;
    }
    base["key"] = generate_sign(sign_params, true);
    std::string body = build_post_body(base, !is_comment);

    auto resp = nlohmann::json::parse(send_post(mod.operate_endpoint, body, ch));
    bool ok = false;
    
    // 检查加密的 data 字段
    if (resp.contains("data") && resp["data"].is_string() && !resp["data"].get<std::string>().empty()) {
        try {
            auto inner = nlohmann::json::parse(decrypt_aes(resp["data"].get<std::string>()));
            if (inner.contains("code")) {
                int c = inner["code"].is_number() ? inner["code"].get<int>()
                    : (inner["code"].is_string() ? safe_stoi(inner["code"].get<std::string>()) : -1);
                if (c == 0 || c == 200) ok = true;
            }
        } catch (...) { /* 解密失败，继续检查外层 code */ }
    }
    
    // 检查外层的 code
    if (!ok && resp.contains("code")) {
        int c = resp["code"].is_number() ? resp["code"].get<int>()
            : (resp["code"].is_string() ? safe_stoi(resp["code"].get<std::string>()) : -1);
        if (c == 0 || c == 200) ok = true;
    }
    return ok;
}

[[nodiscard]] static int extract_level(const nlohmann::json& item) {
    if (item.contains("user_title") && item["user_title"].is_array() && !item["user_title"].empty()) {
        auto& title = item["user_title"][0];
        if (title.contains("txt") && title["txt"].is_string()) {
            std::istringstream iss(title["txt"].get<std::string>());
            std::string word;
            while (iss >> word) {
                if (word.rfind("Lv", 0) == 0 && word.size() > 2) {
                    return safe_stoi(word.substr(2), 0);
                }
            }
        }
    }
    return 0;
}

[[nodiscard]] static std::vector<nlohmann::json> get_join_applications(const ModuleConfig& mod,
    const FamilyConfig& fam, const std::string& token, const std::string& uid, CurlHandle& ch) {
    std::unordered_map<std::string, std::string> params;
    params["family_id"] = fam.family_id;
    params["channel"] = g_config.channel;
    params["version"] = g_config.version;
    params["api_level"] = g_config.api_level;
    params["phone_model"] = g_config.phone_model;
    params["token"] = token;
    params["uid"] = uid;
    params["$*$page"] = g_config.page;
    params["$*$limit"] = g_config.limit;
    params["$*$os_info"] = g_config.os_info;
    params["$*$status"] = mod.state3_pending;
    params["key"] = generate_sign(params, false);

    auto resp = nlohmann::json::parse(send_get(mod.list_endpoint, params, ch));
    std::string d = resp.value("data", "");
    if (d.empty()) return {};
    auto result = nlohmann::json::parse(decrypt_aes(d));
    if (result.contains("code")) {
        int c = result["code"].is_number() ? result["code"].get<int>()
            : (result["code"].is_string() ? safe_stoi(result["code"].get<std::string>()) : -1);
        if (c != 0) return {};
    }
    if (!result.contains("data") || !result["data"].is_array()) return {};
    std::vector<nlohmann::json> items;
    items.reserve(result["data"].size());
    for (auto& item : result["data"]) {
        items.push_back(item);
    }
    return items;
}

[[nodiscard]] static bool operate_join(const ModuleConfig& mod, const nlohmann::json& item,
    const FamilyConfig& fam, const std::string& token, const std::string& uid, CurlHandle& ch,
    const std::string& status, const std::string& msg) {
    std::string id;
    if (item.contains("id")) {
        if (item["id"].is_string()) id = item["id"].get<std::string>();
        else if (item["id"].is_number_integer()) id = std::to_string(item["id"].get<int64_t>());
        else id = item["id"].dump();
    } else return false;

    std::unordered_map<std::string, std::string> sign_params;
    sign_params["uid"] = uid;
    sign_params["api_level"] = g_config.api_level;
    sign_params["family_id"] = fam.family_id;
    sign_params["channel"] = g_config.channel;
    sign_params["id"] = id;
    sign_params["version"] = g_config.version;
    sign_params["phone_model"] = g_config.phone_model;
    sign_params["token"] = token;
    sign_params["status"] = status;
    std::string key = generate_sign(sign_params, false);

    std::string body = "msg=" + msg +
        "&uid=" + uid +
        "&api_level=" + g_config.api_level +
        "&family_id=" + fam.family_id +
        "&os_info=" + g_config.os_info +
        "&channel=" + g_config.channel +
        "&id=" + id +
        "&version=" + g_config.version +
        "&key=" + key +
        "&phone_model=" + g_config.phone_model +
        "&token=" + token +
        "&status=" + status + "&";

    auto resp = nlohmann::json::parse(send_post(mod.operate_endpoint, body, ch));
    bool ok = false;
    if (resp.contains("data") && resp["data"].is_string() && !resp["data"].get<std::string>().empty()) {
        try {
            auto inner = nlohmann::json::parse(decrypt_aes(resp["data"].get<std::string>()));
            if (inner.contains("code") && inner["code"] == 0) ok = true;
        } catch (...) {}
    }
    if (!ok && resp.contains("code")) {
        int c = resp["code"].is_number() ? resp["code"].get<int>()
            : (resp["code"].is_string() ? safe_stoi(resp["code"].get<std::string>()) : -1);
        if (c == 0) ok = true;
    }
    return ok;
}

// ==================== 并发队列 ====================
template<typename T>
class SafeQueue {
    std::deque<T> queue_;
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    bool done_ = false;
public:
    void push(T item) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            queue_.push_back(std::move(item));
        }
        cv_.notify_one();
    }

    bool pop(T& item) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] { return !queue_.empty() || done_; });
        if (queue_.empty()) return false;
        item = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void finish() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            done_ = true;
        }
        cv_.notify_all();
    }
};

// ==================== 日志记录 ====================
static void log_action(const std::string& token, const std::string& family,
    const std::string& type, const std::string& id,
    const std::string& action, const std::string& detail = "") {
    g_log.info("[%s] %s 家族%s\t%s ID=%s -> %s%s",
        now_str().c_str(), mask_token(token).c_str(), family.c_str(),
        type.c_str(), id.c_str(), action.c_str(),
        detail.empty() ? "" : (" | " + escape_log(detail)).c_str());
}

// ==================== UP资源详情 ====================
// GET /up/detail 获取UP资源的详细信息
// 签名公式: MD5("P.8CGq@Wr~Vs]!4!&api_level=...&channel=...&di=...&phone_model=...&sid=...&version=...")
[[nodiscard]] static nlohmann::json get_up_resource_detail(
    const std::string& sid, const std::string& di,
    const std::string& token, const std::string& uid,
    CurlHandle& ch) {
    std::unordered_map<std::string, std::string> params;
    params["channel"] = g_config.channel;
    params["version"] = g_config.version;
    params["api_level"] = g_config.api_level;
    params["phone_model"] = g_config.phone_model;
    params["sid"] = sid;
    params["di"] = di;
    // 以下参数在 URL 中但不参与签名
    params["$*$os_info"] = g_config.os_info;
    params["$*$uid"] = uid;
    params["$*$token"] = token;
    params["$*$isCheck"] = "1";
    params["key"] = generate_sign(params, false);

    auto resp = nlohmann::json::parse(send_get("/up/detail", params, ch));

    // 检查外层 code
    if (resp.contains("code")) {
        int c = resp["code"].is_number() ? resp["code"].get<int>()
            : (resp["code"].is_string() ? safe_stoi(resp["code"].get<std::string>()) : -1);
        if (c != 0) return {};
    }

    // 解密 data
    std::string d = resp.value("data", "");
    if (d.empty()) return {};
    auto result = nlohmann::json::parse(decrypt_aes(d));
    return result;
}

// ==================== UP资源操作提交 ====================
// POST /family/up/check/operate
// 签名公式: MD5("P.8CGq@Wr~Vs]!4!&api_level=...&channel=...&family_id=...&isShow3=1&phone_model=...&status3=...&token=...&uid=...&upId=...&version=...")
[[nodiscard]] static bool operate_up_resource(const ModuleConfig& mod, const std::string& upId,
    const std::string& family_id, const std::string& token, const std::string& uid,
    const std::string& status3, const std::string& msg3, CurlHandle& ch) {
    // 签名参数
    std::unordered_map<std::string, std::string> sign_params;
    sign_params["uid"] = uid;
    sign_params["api_level"] = g_config.api_level;
    sign_params["family_id"] = family_id;
    sign_params["channel"] = g_config.channel;
    sign_params["version"] = g_config.version;
    sign_params["phone_model"] = g_config.phone_model;
    sign_params["token"] = token;
    sign_params["isShow3"] = "1";
    sign_params["status3"] = status3;
    sign_params["upId"] = upId;
    std::string key = generate_sign(sign_params, true);

    // 构造 body（注意末尾 & 是正常的）
    std::string body =
        "upId=" + upId +
        "&msg3=" + url_encode(msg3) +
        "&api_level=" + g_config.api_level +
        "&family_id=" + family_id +
        "&isShow3=1" +
        "&channel=" + g_config.channel +
        "&version=" + g_config.version +
        "&status3=" + status3 +
        "&phone_model=" + url_encode(g_config.phone_model) +
        "&token=" + token +
        "&uid=" + uid +
        "&os_info=" + url_encode(g_config.os_info) +
        "&showmsg3=" +
        "&key=" + key + "&";

    auto resp = nlohmann::json::parse(send_post(mod.operate_endpoint, body, ch));
    bool ok = false;

    // 检查加密的 data 字段
    if (resp.contains("data") && resp["data"].is_string() && !resp["data"].get<std::string>().empty()) {
        try {
            auto inner = nlohmann::json::parse(decrypt_aes(resp["data"].get<std::string>()));
            if (inner.contains("code")) {
                int c = inner["code"].is_number() ? inner["code"].get<int>()
                    : (inner["code"].is_string() ? safe_stoi(inner["code"].get<std::string>()) : -1);
                if (c == 0 || c == 200) ok = true;
            }
        } catch (...) {}
    }

    // 检查外层的 code
    if (!ok && resp.contains("code")) {
        int c = resp["code"].is_number() ? resp["code"].get<int>()
            : (resp["code"].is_string() ? safe_stoi(resp["code"].get<std::string>()) : -1);
        if (c == 0 || c == 200) ok = true;
    }
    return ok;
}

// ==================== UP资源审核处理 ====================
static void process_up_resource_items(const ModuleConfig& mod, TokenReviewer* rev,
    std::vector<std::unique_ptr<PendingItem>>& items,
    int concurrency, int delay_ms, int max_coin = -1,
    bool ereg = false, bool ebad = true) {
    if (items.empty()) return;
    if (concurrency <= 0) concurrency = 5;
    if (concurrency > static_cast<int>(items.size())) concurrency = items.size();

    SafeQueue<PendingItem*> queue;
    std::vector<std::thread> threads(concurrency);
    std::atomic<int32_t> approved{0}, rejected{0}, failed{0};

    for (int i = 0; i < concurrency; ++i) {
        threads[i] = std::thread([&, mod]() {
            CurlHandlePtr ch = acquire_curl();
            PendingItem* item = nullptr;

            while (queue.pop(item) && g_running) {
                try {
                    // 1. 获取详情
                    auto detail = get_up_resource_detail(item->id, item->di, item->token, item->uid, *ch);
                    if (detail.empty() || !detail.contains("data")) {
                        g_log.error("[%s] %s 家族%s\tUP资源 ID=%s 获取详情失败",
                            now_str().c_str(), mask_token(item->token).c_str(),
                            item->family_id.c_str(), item->id.c_str());
                        failed++;
                        if (delay_ms > 0)
                            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                        continue;
                    }

                    auto& data = detail["data"];

                    // 2. 提取文字做敏感词检查
                    std::string up_text;
                    // 提取用户自定义文本字段（跳过平台固定值：typeName/lang/状态名/平台提示等）
                    std::vector<std::string> text_fields = {
                        "varName",             // 资源名称（最重要）
                        "sourceName",          // 来源名称
                        "sourceDescription",   // 来源描述
                        "sourceCharacteristic",// 来源特点描述
                        "notename",            // 备注
                        "msg3"                 // 拒绝原因/自定义消息
                    };
                    for (auto& f : text_fields) {
                        if (data.contains(f) && data[f].is_string()) {
                            std::string v = data[f].get<std::string>();
                            if (!v.empty()) {
                                if (!up_text.empty()) up_text += "\n";
                                up_text += v;
                            }
                        }
                    }
                    // 额外提取嵌套的 family.family_name
                    if (data.contains("family") && data["family"].is_object()) {
                        auto& family = data["family"];
                        if (family.contains("family_name") && family["family_name"].is_string()) {
                            std::string v = family["family_name"].get<std::string>();
                            if (!v.empty()) {
                                if (!up_text.empty()) up_text += "\n";
                                up_text += v;
                            }
                        }
                    }

                    bool should_approve = true;
                    std::string reason;

                    if (!up_text.empty()) {
                        TextChecker checker(up_text);
                        auto cr = check_text(checker, ereg, ebad,
                            item->family_id, "up_resource");
                        if (cr.should_reject) {
                            should_approve = false;
                            reason = cr.reason;
                        }
                    }

                    // 3. 检查 needCoin
                    int need_coin = 0;
                    if (data.contains("needCoin") && data["needCoin"].is_number()) {
                        need_coin = data["needCoin"].get<int>();
                    }
                    if (should_approve && max_coin >= 0 && need_coin > max_coin) {
                        should_approve = false;
                        reason = "needCoin=" + std::to_string(need_coin) + " > 最大允许=" + std::to_string(max_coin);
                    }

                    // 4. 提交审核结果
                    bool sent = false;
                    if (should_approve) {
                        sent = operate_up_resource(mod, item->id, item->family_id,
                            item->token, item->uid,
                            mod.state3_approved, "", *ch);
                        log_action(item->token, item->family_id, "UP资源",
                            item->id, sent ? "通过" : "通过失败");
                        if (sent) {
                            approved++;
                            g_plugin_mgr.dispatch_item_approved(
                                item->family_id, "up_resource", item->id);
                        }
                    } else {
                        sent = operate_up_resource(mod, item->id, item->family_id,
                            item->token, item->uid,
                            mod.state3_rejected, reason, *ch);
                        log_action(item->token, item->family_id, "UP资源",
                            item->id, sent ? "拒绝" : "拒绝失败", reason);
                        if (sent) {
                            rejected++;
                            g_plugin_mgr.dispatch_item_rejected(
                                item->family_id, "up_resource", item->id, reason);
                        }
                    }

                    if (!sent) failed++;
                } catch (const std::exception& e) {
                    g_plugin_mgr.dispatch_error(
                        item->family_id, "up_resource", e.what());
                    g_log.error("[%s] %s 家族%s\tUP资源 ID=%s 异常: %s",
                        now_str().c_str(), mask_token(item->token).c_str(),
                        item->family_id.c_str(), item->id.c_str(), e.what());
                    failed++;
                }
                if (delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
        });
    }

    for (auto& item : items) {
        if (g_running) queue.push(item.get());
    }
    queue.finish();
    for (auto& t : threads) t.join();

    rev->add_up_resource_total(items[0]->family_id, items.size());
    rev->add_up_resource_approved(items[0]->family_id, approved);
    rev->add_up_resource_rejected(items[0]->family_id, rejected);
    if (failed > 0) {
        g_log.error("[%s] %s 家族%s UP资源操作失败数: %d",
            now_str().c_str(), mask_token(items[0]->token).c_str(),
            items[0]->family_id.c_str(), failed.load());
    }
}

// ==================== 并发处理 ====================
struct ItemTask {
    enum Type { REJECT, APPROVE };
    Type type;
    PendingItem* item;
    std::string reason;
};

static void process_items(const ModuleConfig& mod, TokenReviewer* rev,
    std::vector<std::unique_ptr<PendingItem>>& items, bool is_comment,
    int concurrency, int delay_ms, bool ereg, bool ebad,
    const char* stats_type = nullptr) {
    if (items.empty()) return;
    if (concurrency <= 0) concurrency = 5;
    if (concurrency > static_cast<int>(items.size())) concurrency = items.size();

    std::string type_label = is_comment ? "评论" : "帖子";
    if (stats_type) type_label = stats_type;

    std::vector<ItemTask> tasks;
    tasks.reserve(items.size());
    for (auto& item : items) {
        TextChecker checker(item->full_text);
        std::string type_for_check = stats_type ? std::string(stats_type) :
            (is_comment ? "comment" : "post");
        auto cr = check_text(checker, ereg, ebad,
            item->family_id, type_for_check);
        if (cr.should_reject) {
            tasks.push_back({ItemTask::REJECT, item.get(), cr.reason});
        } else {
            tasks.push_back({ItemTask::APPROVE, item.get(), ""});
        }
    }

    SafeQueue<ItemTask*> queue;
    std::vector<std::thread> threads(concurrency);

    for (int i = 0; i < concurrency; ++i) {
        threads[i] = std::thread([&, mod]() {
            CurlHandlePtr ch = acquire_curl();
            ItemTask* task = nullptr;

            while (queue.pop(task) && g_running) {
                try {
                    bool sent = false;
                    if (task->type == ItemTask::REJECT) {
                        sent = approve_item(mod, task->item->id, task->item->family_id,
                            task->item->token, task->item->uid,
                            mod.state3_rejected, is_comment, *ch);
                        log_action(task->item->token, task->item->family_id, type_label,
                            task->item->id, sent ? "拒绝" : "拒绝失败", task->reason);
                        if (sent) {
                            if (stats_type) {
                                if (strcmp(stats_type, "UP资源") == 0) {
                                    rev->add_up_resource_rejected(task->item->family_id, 1);
                                }
                            } else if (is_comment) {
                                rev->add_comment_rejected(task->item->family_id, 1);
                            } else {
                                rev->add_post_rejected(task->item->family_id, 1);
                            }
                            g_plugin_mgr.dispatch_item_rejected(
                                task->item->family_id, type_label,
                                task->item->id, task->reason);
                        }
                    } else {
                        sent = approve_item(mod, task->item->id, task->item->family_id,
                            task->item->token, task->item->uid,
                            mod.state3_approved, is_comment, *ch);
                        log_action(task->item->token, task->item->family_id, type_label,
                            task->item->id, sent ? "通过" : "通过失败");
                        if (sent) {
                            if (stats_type) {
                                if (strcmp(stats_type, "UP资源") == 0) {
                                    rev->add_up_resource_approved(task->item->family_id, 1);
                                }
                            } else if (is_comment) {
                                rev->add_comment_approved(task->item->family_id, 1);
                            } else {
                                rev->add_post_approved(task->item->family_id, 1);
                            }
                            g_plugin_mgr.dispatch_item_approved(
                                task->item->family_id, type_label,
                                task->item->id);
                        }
                    }
                } catch (const std::exception& e) {
                    g_plugin_mgr.dispatch_error(task->item->family_id, type_label, e.what());
                    g_log.error("[%s] %s 家族%s\t%s ID=%s HTTP异常: %s",
                        now_str().c_str(), mask_token(task->item->token).c_str(),
                        task->item->family_id.c_str(), type_label.c_str(),
                        task->item->id.c_str(), e.what());
                }
                if (delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
        });
    }

    for (auto& task : tasks) {
        if (g_running) queue.push(&task);
    }
    queue.finish();
    for (auto& t : threads) t.join();

    if (stats_type) {
        if (strcmp(stats_type, "UP资源") == 0) {
            rev->add_up_resource_total(items[0]->family_id, items.size());
        }
    } else if (is_comment) {
        rev->add_comment_total(items[0]->family_id, items.size());
    } else {
        rev->add_post_total(items[0]->family_id, items.size());
    }
}

static void process_up_items(const ModuleConfig& mod, TokenReviewer* rev,
    std::vector<std::unique_ptr<PendingItem>>& items,
    int concurrency, int delay_ms, bool ereg, bool ebad) {
    if (items.empty()) return;
    if (concurrency <= 0) concurrency = 5;
    if (concurrency > static_cast<int>(items.size())) concurrency = items.size();

    std::vector<ItemTask> tasks;
    tasks.reserve(items.size());
    for (auto& item : items) {
        TextChecker checker(item->full_text);
        auto cr = check_text(checker, ereg, ebad,
            item->family_id, "up");
        if (cr.should_reject) {
            tasks.push_back({ItemTask::REJECT, item.get(), cr.reason});
        } else {
            tasks.push_back({ItemTask::APPROVE, item.get(), ""});
        }
    }

    SafeQueue<ItemTask*> queue;
    std::vector<std::thread> threads(concurrency);

    for (int i = 0; i < concurrency; ++i) {
        threads[i] = std::thread([&, mod]() {
            CurlHandlePtr ch = acquire_curl();
            ItemTask* task = nullptr;

            while (queue.pop(task) && g_running) {
                try {
                    bool sent = false;
                    if (task->type == ItemTask::REJECT) {
                        sent = approve_item(mod, task->item->id, task->item->family_id,
                            task->item->token, task->item->uid,
                            mod.state3_rejected, true, *ch);
                        log_action(task->item->token, task->item->family_id, "UP评论",
                            task->item->id, sent ? "拒绝" : "拒绝失败", task->reason);
                        if (sent) {
                            rev->add_up_rejected(task->item->family_id, 1);
                            g_plugin_mgr.dispatch_item_rejected(
                                task->item->family_id, "up", task->item->id, task->reason);
                        }
                    } else {
                        sent = approve_item(mod, task->item->id, task->item->family_id,
                            task->item->token, task->item->uid,
                            mod.state3_approved, true, *ch);
                        log_action(task->item->token, task->item->family_id, "UP评论",
                            task->item->id, sent ? "通过" : "通过失败");
                        if (sent) {
                            rev->add_up_approved(task->item->family_id, 1);
                            g_plugin_mgr.dispatch_item_approved(
                                task->item->family_id, "up", task->item->id);
                        }
                    }
                } catch (const std::exception& e) {
                    g_plugin_mgr.dispatch_error(task->item->family_id, "up", e.what());
                    g_log.error("[%s] %s 家族%s\tUP评论 ID=%s HTTP异常: %s",
                        now_str().c_str(), mask_token(task->item->token).c_str(),
                        task->item->family_id.c_str(),
                        task->item->id.c_str(), e.what());
                }
                if (delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
        });
    }

    for (auto& task : tasks) {
        if (g_running) queue.push(&task);
    }
    queue.finish();
    for (auto& t : threads) t.join();

    rev->add_up_total(items[0]->family_id, items.size());
}

static void process_join_items(const ModuleConfig& mod, TokenReviewer* rev,
    const FamilyConfig& fam, std::vector<nlohmann::json>& items,
    int concurrency, int delay_ms) {
    if (items.empty()) return;
    if (concurrency <= 0) concurrency = 5;
    if (concurrency > static_cast<int>(items.size())) concurrency = items.size();

    std::atomic<int32_t> approved{0}, rejected{0}, failed{0};
    SafeQueue<nlohmann::json*> queue;
    std::vector<std::thread> threads(concurrency);

    for (int i = 0; i < concurrency; ++i) {
        threads[i] = std::thread([&, mod]() {
            CurlHandlePtr ch = acquire_curl();
            nlohmann::json* item = nullptr;

            while (queue.pop(item) && g_running) {
                std::string id;
                try {
                    int level = extract_level(*item);
                    if ((*item).contains("id")) {
                        if ((*item)["id"].is_string()) id = (*item)["id"].get<std::string>();
                        else if ((*item)["id"].is_number_integer()) id = std::to_string((*item)["id"].get<int64_t>());
                        else id = (*item)["id"].dump();
                    }
                    bool should_reject = (fam.min_level != -1 && level < fam.min_level);
                    std::string status = should_reject ? mod.state3_rejected : mod.state3_approved;
                    std::string msg = should_reject ? "等级不够" : "";
                    bool ok = operate_join(mod, *item, fam, rev->token, rev->uid, *ch, status, msg);
                    if (ok) {
                        if (should_reject) rejected++; else approved++;
                        log_action(rev->token, fam.family_id, "加入", id,
                            ok ? (should_reject ? "拒绝" : "通过") : "操作失败",
                            should_reject ? "等级不足 Lv" + std::to_string(level) : "");
                    } else {
                        failed++;
                        log_action(rev->token, fam.family_id, "加入", id, "操作失败");
                    }
                } catch (const std::exception& e) {
                    g_log.error("[%s] %s 家族%s\t加入 ID=%s HTTP异常: %s",
                        now_str().c_str(), mask_token(rev->token).c_str(),
                        fam.family_id.c_str(), id.c_str(), e.what());
                    failed++;
                }
                if (delay_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                }
            }
        });
    }

    for (auto& item : items) {
        if (g_running) queue.push(&item);
    }
    queue.finish();
    for (auto& t : threads) t.join();

    rev->add_join_total(fam.family_id, items.size());
    rev->add_join_approved(fam.family_id, approved);
    rev->add_join_rejected(fam.family_id, rejected);
    if (failed > 0) {
        g_log.error("[%s] %s 家族%s 加入操作失败数: %d",
            now_str().c_str(), mask_token(rev->token).c_str(), fam.family_id.c_str(), failed.load());
    }
}

// ==================== 家族循环 ====================
static void family_loop(TokenReviewer* rev, const FamilyConfig& fam) {
    bool ereg = g_config.enable_regex;
    bool ebad = true;
    TokenConfig* tc = nullptr;
    for (auto& t : g_config.tokens) {
        if (t.token == rev->token) { tc = &t; break; }
    }
    int concurrency = (tc && tc->concurrency) ? *tc->concurrency : g_config.concurrency;
    int delay_ms = (tc && tc->request_delay_ms) ? *tc->request_delay_ms : g_config.request_delay_ms;
    if (tc) {
        if (tc->enable_regex) ereg = *tc->enable_regex;
        if (tc->enable_bad_words) ebad = *tc->enable_bad_words;
    }

    std::string masked_token = mask_token(rev->token);

    // 获取家族控制标志
    auto control = g_family_controls[fam.family_id];
    if (!control) {
        control = std::make_shared<FamilyControl>();
        g_family_controls[fam.family_id] = control;
    }

    g_log.info("[%s] %s 家族%s\t🚀 启动 (并发=%d 延迟=%dms)",
        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), concurrency, delay_ms);

    while (g_running) {
        g_plugin_mgr.dispatch_review_round_start(fam.family_id);
        // 暂停检查 (一次运行模式跳过)
        // 使用状态机防止 TOCTOU 竞态：
        //   0 = 运行中, 1 = 暂停中, 2 = 已暂停, 3 = 恢复中
        if (!g_once_mode) {
            int old_state = 0;
            if (control->paused.load()) {
                // 进入暂停
                control->paused.store(2);  // 标记为已暂停
                old_state = 2;
                g_plugin_mgr.dispatch_pause(fam.family_id);
            }
            while (control->paused.load() != 0 && g_running) {
                control->last_activity = time(nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            // 如果之前是暂停状态且现在不再暂停，才发送恢复通知
            if (old_state == 2 && control->paused.load() == 0) {
                g_plugin_mgr.dispatch_resume(fam.family_id);
            }
            if (!g_running) break;
        }

        bool work = false;
        CurlHandlePtr ch = acquire_curl();

        if (g_config.post.enabled && fam.enable_post) {
            try {
                auto items = get_pending_items(g_config.post, false, fam, rev->token, rev->uid, *ch);
                if (!items.empty()) {
                    work = true;
                    g_log.info("[%s] %s 家族%s\t📋 帖子: %zu",
                        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), items.size());
                    process_items(g_config.post, rev, items, false, concurrency, delay_ms, ereg, ebad);
                    if (delay_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    }
                }
            } catch (const std::exception& e) {
                g_plugin_mgr.dispatch_error(fam.family_id, "post", e.what());
                g_log.error("[%s] %s 家族%s\t❌ 帖子: %s",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), e.what());
            }
        }

        if (g_config.comment.enabled && fam.enable_comment) {
            try {
                auto items = get_pending_items(g_config.comment, true, fam, rev->token, rev->uid, *ch);
                if (!items.empty()) {
                    work = true;
                    g_log.info("[%s] %s 家族%s\t📋 评论: %zu",
                        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), items.size());
                    process_items(g_config.comment, rev, items, true, concurrency, delay_ms, ereg, ebad);
                    if (delay_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    }
                }
            } catch (const std::exception& e) {
                g_plugin_mgr.dispatch_error(fam.family_id, "comment", e.what());
                g_log.error("[%s] %s 家族%s\t❌ 评论: %s",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), e.what());
            }
        }

        if (g_config.join.enabled && fam.enable_join) {
            try {
                auto items = get_join_applications(g_config.join, fam, rev->token, rev->uid, *ch);
                if (!items.empty()) {
                    work = true;
                    g_log.info("[%s] %s 家族%s\t📋 加入: %zu",
                        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), items.size());
                    process_join_items(g_config.join, rev, fam, items, concurrency, delay_ms);
                    if (delay_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    }
                }
            } catch (const std::exception& e) {
                g_plugin_mgr.dispatch_error(fam.family_id, "join", e.what());
                g_log.error("[%s] %s 家族%s\t❌ 加入: %s",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), e.what());
            }
        }

        if (g_config.up.enabled && fam.enable_up) {
            try {
                auto items = get_pending_items(g_config.up, true, fam, rev->token, rev->uid, *ch);
                if (!items.empty()) {
                    work = true;
                    g_log.info("[%s] %s 家族%s\t📋 UP评论: %zu",
                        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), items.size());
                    process_up_items(g_config.up, rev, items, concurrency, delay_ms, ereg, ebad);
                    if (delay_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    }
                }
            } catch (const std::exception& e) {
                g_plugin_mgr.dispatch_error(fam.family_id, "up", e.what());
                g_log.error("[%s] %s 家族%s\t❌ UP评论: %s",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), e.what());
            }
        }

        if (g_config.up_resource.enabled && fam.enable_up_resource) {
            try {
                auto items = get_pending_items(g_config.up_resource, false, fam, rev->token, rev->uid, *ch);
                if (!items.empty()) {
                    work = true;
                    g_log.info("[%s] %s 家族%s\t📋 UP资源: %zu",
                        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), items.size());
                    int fam_max_coin = (fam.max_up_resource_coin >= 0)
                        ? fam.max_up_resource_coin
                        : g_config.max_up_resource_coin;
                    process_up_resource_items(g_config.up_resource, rev, items, concurrency, delay_ms, fam_max_coin, ereg, ebad);
                    if (delay_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
                    }
                }
            } catch (const std::exception& e) {
                g_plugin_mgr.dispatch_error(fam.family_id, "up_resource", e.what());
                g_log.error("[%s] %s 家族%s\t❌ UP资源: %s",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), e.what());
            }
        }

        ch.reset();  // 归还句柄到池

        // 一次运行模式: 执行一次即退出
        if (g_once_mode) {
            g_log.info("[%s] %s 家族%s\t一次运行完成",
                now_str().c_str(), masked_token.c_str(), fam.family_id.c_str());
            break;
        }

        // 检查配置版本号，如果有更新则重载热配置
        if (g_config_version.load() != 0) {
            g_log.info("[%s] %s 家族%s\t♻️ 检测到配置更新，重载中...",
                now_str().c_str(), masked_token.c_str(), fam.family_id.c_str());
            // 重新解析配置文件，更新可热重载字段
            try {
                auto new_data = toml::parse(g_config_path);
                if (new_data.contains("CONCURRENCY")) {
                    int new_c = toml::find<int>(new_data, "CONCURRENCY");
                    g_config.concurrency = new_c;
                }
                if (new_data.contains("REQUEST_DELAY_MS")) {
                    int new_d = toml::find<int>(new_data, "REQUEST_DELAY_MS");
                    g_config.request_delay_ms = new_d;
                }
                if (new_data.contains("ENABLE_REGEX")) {
                    g_config.enable_regex = toml::find<bool>(new_data, "ENABLE_REGEX");
                }
                if (new_data.contains("MAX_UP_RESOURCE_COIN")) {
                    g_config.max_up_resource_coin = toml::find<int>(new_data, "MAX_UP_RESOURCE_COIN");
                }
                if (new_data.contains("CHECK_INTERVAL_SECONDS")) {
                    g_config.check_interval_seconds = toml::find<int>(new_data, "CHECK_INTERVAL_SECONDS");
                }
                g_log.info("[%s] %s 家族%s\t✅ 配置热重载完成",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str());
            } catch (const std::exception& e) {
                g_log.error("[%s] %s 家族%s\t❌ 配置热重载失败: %s",
                    now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), e.what());
            }
            g_config_version.store(0);  // 重置标记
        }

        g_plugin_mgr.dispatch_review_round_end(fam.family_id, 0, 0, 0);

        if (!work) {
            int total = g_config.check_interval_seconds;
            for (int i = 0; i < total && g_running; ++i) {
                int remain = total - i;
                if (remain % 60 == 0 || i == 0) {
                    g_log.info("[%s] %s 家族%s\t⏳ %ds",
                        now_str().c_str(), masked_token.c_str(), fam.family_id.c_str(), remain);
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }
    if (!g_once_mode) {
        g_log.info("[%s] %s 家族%s\t🏁 停止",
            now_str().c_str(), masked_token.c_str(), fam.family_id.c_str());
    }
}

// ==================== 配置加载 ====================
static void load_config(const std::string& path = "settings.toml") {
    auto data = toml::parse(path);
    g_config.sign_const = toml::find<std::string>(data, "SIGN_CONST");
    g_config.base_url = toml::find<std::string>(data, "BASE_URL");
    g_config.api_level = toml::find<std::string>(data, "API_LEVEL");
    g_config.version = toml::find<std::string>(data, "VERSION");
    g_config.channel = toml::find<std::string>(data, "CHANNEL");
    g_config.phone_model = toml::find<std::string>(data, "PHONE_MODEL");
    g_config.os_info = toml::find<std::string>(data, "OS_INFO");
    g_config.page = toml::find<std::string>(data, "PAGE");
    g_config.limit = toml::find<std::string>(data, "LIMIT");
    g_config.check_interval_seconds = toml::find<int>(data, "CHECK_INTERVAL_SECONDS");
    g_config.request_delay_ms = toml::find<int>(data, "REQUEST_DELAY_MS");
    g_config.concurrency = toml::find<int>(data, "CONCURRENCY");
    g_config.max_up_resource_coin = toml::find<int>(data, "MAX_UP_RESOURCE_COIN");
    g_config.user_agent = toml::find<std::string>(data, "USER_AGENT");
    if (data.contains("ENABLE_REGEX")) {
        g_config.enable_regex = toml::find<bool>(data, "ENABLE_REGEX");
    }
    if (data.contains("THEME")) {
        auto t = toml::find<std::string>(data, "THEME");
        if (t == "dark") g_theme = Theme::DARK;
        else if (t == "light") g_theme = Theme::LIGHT;
    }

    if (data.contains("TUI")) {
        auto& tui_sec = data.at("TUI");
        g_config.tui_enabled = toml::find<bool>(tui_sec, "ENABLED");
    }

    if (data.contains("WEB")) {
        auto& web_sec = data.at("WEB");
        g_config.web.enabled = toml::find<bool>(web_sec, "ENABLED");
        g_config.web.port = toml::find<int>(web_sec, "PORT");
        g_config.web.bind = toml::find<std::string>(web_sec, "BIND");
        g_config.web.root = toml::find<std::string>(web_sec, "ROOT");
    }

    auto load_mod = [&](const std::string& key, ModuleConfig& mod) {
        auto& sec = toml::find(data, key);
        mod.enabled = toml::find<bool>(sec, "ENABLED");
        mod.list_endpoint = toml::find<std::string>(sec, "LIST_ENDPOINT");
        mod.operate_endpoint = toml::find<std::string>(sec, "OPERATE_ENDPOINT");
        mod.state3_pending = toml::find<std::string>(sec, "STATE3_PENDING");
        mod.state3_approved = toml::find<std::string>(sec, "STATE3_APPROVED");
        mod.state3_rejected = toml::find<std::string>(sec, "STATE3_REJECTED");
    };
    load_mod("POST", g_config.post);
    load_mod("COMMENT", g_config.comment);
    load_mod("JOIN", g_config.join);
    load_mod("UP", g_config.up);
    load_mod("UP_RESOURCE", g_config.up_resource);
    g_config.up_resource.use_status3 = true;

    for (auto& tok : toml::find(data, "TOKENS").as_array()) {
        TokenConfig tc;
        tc.token = toml::find<std::string>(tok, "TOKEN");
        tc.uid = toml::find<std::string>(tok, "UID");
        if (tok.contains("ENABLE_REGEX")) {
            tc.enable_regex = toml::find<bool>(tok, "ENABLE_REGEX");
        }
        if (tok.contains("ENABLE_BADWORDS")) {
            tc.enable_bad_words = toml::find<bool>(tok, "ENABLE_BADWORDS");
        }
        if (tok.contains("CONCURRENCY")) {
            tc.concurrency = toml::find<int>(tok, "CONCURRENCY");
        }
        if (tok.contains("REQUEST_DELAY_MS")) {
            tc.request_delay_ms = toml::find<int>(tok, "REQUEST_DELAY_MS");
        }
        for (auto& fam : toml::find(tok, "FAMILIES").as_array()) {
            FamilyConfig fc;
            fc.family_id = toml::find<std::string>(fam, "FAMILY_ID");
            fc.mid = toml::find<std::string>(fam, "MID");
            fc.enable_post = toml::find<bool>(fam, "ENABLE_POST");
            fc.enable_comment = toml::find<bool>(fam, "ENABLE_COMMENT");
            fc.enable_join = toml::find<bool>(fam, "ENABLE_JOIN");
            fc.enable_up = toml::find<bool>(fam, "ENABLE_UP");
            fc.enable_up_resource = toml::find<bool>(fam, "ENABLE_UP_RESOURCE");
            fc.max_up_resource_coin = toml::find<int>(fam, "MAX_UP_RESOURCE_COIN");
            fc.min_level = toml::find<int>(fam, "MIN_LEVEL");
            if (fam.contains("PLUGINS_DIR")) {
                auto& arr = fam.at("PLUGINS_DIR").as_array();
                for (auto& d : arr) {
                    fc.plugins_dirs.push_back(std::string(d.as_string()));
                }
            }
            tc.families.push_back(fc);
        }
        g_config.tokens.push_back(tc);
    }
}

static void load_bad_words() {
    std::ifstream f("words.json");
    if (!f) return;
    auto j = nlohmann::json::parse(f);
    g_bad_words.enabled = j.value("enabled", false);
    g_bad_words.words = j.value("words", std::vector<std::string>{});
    for (auto& w : g_bad_words.words) {
        std::transform(w.begin(), w.end(), w.begin(), ::tolower);
    }
    g_bad_words.build_ac_matcher();
    g_log.info("✅ 普通违禁词:\t%zu 条", g_bad_words.words.size());
}

static void load_regex_words() {
    std::ifstream f("words_.json");
    if (!f) return;
    auto j = nlohmann::json::parse(f);
    g_regex_data.enabled = j.value("enabled", false) && g_config.enable_regex;
    for (auto& pattern : j.value("patterns", std::vector<std::string>{})) {
        int errcode;
        PCRE2_SIZE erroffset;
        pcre2_code* re_raw = pcre2_compile(
            (PCRE2_SPTR)pattern.c_str(), pattern.size(), 0,
            &errcode, &erroffset, nullptr);
        if (re_raw != nullptr) {
            g_regex_data.patterns.push_back(Pcre2CodePtr(re_raw));
        } else {
            g_log.error("⚠️ 正则错误: %s", pattern.c_str());
        }
    }
    g_log.info("✅ 正则违禁词:\t%zu 条", g_regex_data.patterns.size());
}

// ==================== 配置迁移工具 ====================
// 读取旧的 settings.toml，生成规范化新配置，保留所有用户数据
// 兼容 toml11 v3 和 v4 (不使用 format/ordered_type_config，不引用 toml::value 类型)

static std::string toml_escape(const std::string& s) {
    std::string out;
    out += '"';
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    out += '"';
    return out;
}

static bool migrate_config(const std::string& old_path, const std::string& new_path) {
    // 解析旧配置（auto 推导类型，兼容 v3/v4）
    auto old = toml::parse(old_path);

    if (!old.is_table()) {
        g_log.error("配置迁移: 旧配置根不是表");
        return false;
    }

    auto has = [&](const auto& tbl, const std::string& key) -> bool {
        if (!tbl.is_table()) return false;
        try { tbl.at(key); return true; } catch (...) { return false; }
    };

    auto get_str = [&](const auto& tbl, const std::string& key, const std::string& def) -> std::string {
        if (!tbl.is_table()) return def;
        try { const auto& v = tbl.at(key); return v.is_string() ? std::string(v.as_string()) : def; }
        catch (...) { return def; }
    };

    auto get_int = [&](const auto& tbl, const std::string& key, std::int64_t def) -> std::int64_t {
        if (!tbl.is_table()) return def;
        try { const auto& v = tbl.at(key); return v.is_integer() ? v.as_integer() : def; }
        catch (...) { return def; }
    };

    auto get_bool = [&](const auto& tbl, const std::string& key, bool def) -> bool {
        if (!tbl.is_table()) return def;
        try { const auto& v = tbl.at(key); return v.is_boolean() ? v.as_boolean() : def; }
        catch (...) { return def; }
    };

    std::ostringstream out;

    auto write_str = [&](const std::string& k, const std::string& v) { out << k << " = " << toml_escape(v) << '\n'; };
    auto write_int = [&](const std::string& k, std::int64_t v)       { out << k << " = " << v << '\n'; };
    auto write_bool = [&](const std::string& k, bool v)              { out << k << " = " << (v ? "true" : "false") << '\n'; };

    // ===== 顶层标量 =====
    write_str("SIGN_CONST",           get_str(old, "SIGN_CONST", ""));
    write_str("API_LEVEL",           get_str(old, "API_LEVEL", "36"));
    write_str("LIMIT",               get_str(old, "LIMIT", "50"));
    write_str("BASE_URL",            get_str(old, "BASE_URL", ""));
    write_str("CHANNEL",             get_str(old, "CHANNEL", "rtk"));
    write_str("USER_AGENT",          get_str(old, "USER_AGENT", ""));
    write_str("VERSION",             get_str(old, "VERSION", ""));
    write_str("PHONE_MODEL",         get_str(old, "PHONE_MODEL", ""));
    write_int("REQUEST_DELAY_MS",    get_int(old, "REQUEST_DELAY_MS", 100));
    write_str("OS_INFO",             get_str(old, "OS_INFO", ""));
    write_str("PAGE",                get_str(old, "PAGE", "1"));
    write_bool("ENABLE_REGEX",        get_bool(old, "ENABLE_REGEX", true));
    write_int("CHECK_INTERVAL_SECONDS", get_int(old, "CHECK_INTERVAL_SECONDS", 300));
    write_int("CONCURRENCY",         get_int(old, "CONCURRENCY", 2));
    write_int("MAX_UP_RESOURCE_COIN", get_int(old, "MAX_UP_RESOURCE_COIN", -1));
    write_str("THEME",               get_str(old, "THEME", "tokyo-night"));
    out << '\n';

    // ===== [TUI] =====
    out << "[TUI]\n";
    write_bool("ENABLED", has(old, "TUI") ? get_bool(old.at("TUI"), "ENABLED", true) : true);
    out << '\n';

    // ===== [WEB] =====
    out << "[WEB]\n";
    if (has(old, "WEB")) {
        const auto& w = old.at("WEB");
        write_bool("ENABLED", get_bool(w, "ENABLED", false));
        write_int("PORT",     get_int(w, "PORT", 2356));
        write_str("BIND",     get_str(w, "BIND", "127.0.0.1"));
        write_str("ROOT",     get_str(w, "ROOT", "./web"));
    } else {
        write_bool("ENABLED", false);
        write_int("PORT",     2356);
        write_str("BIND",     "127.0.0.1");
        write_str("ROOT",     "./web");
    }
    out << '\n';

    // ===== [[TOKENS]] =====
    if (has(old, "TOKENS")) {
        for (const auto& tok : old.at("TOKENS").as_array()) {
            if (!tok.is_table()) continue;
            out << "[[TOKENS]]\n";
            write_str("TOKEN",            get_str(tok, "TOKEN", ""));
            write_str("UID",              get_str(tok, "UID", ""));
            write_bool("ENABLE_REGEX",     get_bool(tok, "ENABLE_REGEX", true));
            write_bool("ENABLE_BADWORDS",  get_bool(tok, "ENABLE_BADWORDS", true));
            write_int("CONCURRENCY",       get_int(tok, "CONCURRENCY", 2));
            write_int("REQUEST_DELAY_MS",  get_int(tok, "REQUEST_DELAY_MS", 100));

            // [[TOKENS.FAMILIES]]
            if (has(tok, "FAMILIES")) {
                for (const auto& fam : tok.at("FAMILIES").as_array()) {
                    if (!fam.is_table()) continue;
                    out << "[[TOKENS.FAMILIES]]\n";
                    write_str("FAMILY_ID",            get_str(fam, "FAMILY_ID", ""));
                    write_str("MID",                  get_str(fam, "MID", ""));
                    write_bool("ENABLE_POST",          get_bool(fam, "ENABLE_POST", false));
                    write_bool("ENABLE_COMMENT",       get_bool(fam, "ENABLE_COMMENT", false));
                    write_bool("ENABLE_JOIN",          get_bool(fam, "ENABLE_JOIN", false));
                    write_bool("ENABLE_UP",            get_bool(fam, "ENABLE_UP", false));
                    write_bool("ENABLE_UP_RESOURCE",   get_bool(fam, "ENABLE_UP_RESOURCE", false));
                    write_int("MAX_UP_RESOURCE_COIN",  get_int(fam, "MAX_UP_RESOURCE_COIN", -1));
                    write_int("MIN_LEVEL",             get_int(fam, "MIN_LEVEL", -1));

                    // PLUGINS_DIR (optional array)
                    if (has(fam, "PLUGINS_DIR")) {
                        const auto& arr = fam.at("PLUGINS_DIR").as_array();
                        if (!arr.empty()) {
                            out << "PLUGINS_DIR = [";
                            bool first = true;
                            for (const auto& d : arr) {
                                if (!first) out << ", ";
                                first = false;
                                out << toml_escape(d.is_string() ? d.as_string() : "");
                            }
                            out << "]\n";
                        }
                    }
                    out << '\n';
                }
            }
        }
    }

    // ===== Module 段 =====
    auto write_mod = [&](const std::string& name) {
        out << '[' << name << "]\n";
        if (has(old, name)) {
            const auto& s = old.at(name);
            write_bool("ENABLED",          get_bool(s, "ENABLED", false));
            write_str("LIST_ENDPOINT",     get_str(s, "LIST_ENDPOINT", ""));
            write_str("OPERATE_ENDPOINT",  get_str(s, "OPERATE_ENDPOINT", ""));
            write_str("STATE3_PENDING",    get_str(s, "STATE3_PENDING", ""));
            write_str("STATE3_APPROVED",   get_str(s, "STATE3_APPROVED", ""));
            write_str("STATE3_REJECTED",   get_str(s, "STATE3_REJECTED", ""));
        } else {
            write_bool("ENABLED",          false);
            write_str("LIST_ENDPOINT",     "");
            write_str("OPERATE_ENDPOINT",  "");
            write_str("STATE3_PENDING",    "");
            write_str("STATE3_APPROVED",   "");
            write_str("STATE3_REJECTED",   "");
        }
        out << '\n';
    };
    write_mod("JOIN");
    write_mod("UP");
    write_mod("UP_RESOURCE");
    write_mod("POST");
    write_mod("COMMENT");

    // ===== [PLUGINS] =====
    out << "[PLUGINS]\n";
    out << "dirs = [";
    if (has(old, "PLUGINS") && has(old.at("PLUGINS"), "dirs")) {
        bool first = true;
        for (const auto& d : old.at("PLUGINS").at("dirs").as_array()) {
            if (!first) out << ", ";
            first = false;
            out << toml_escape(d.is_string() ? d.as_string() : "");
        }
    } else {
        out << toml_escape("./plugins") << ", " << toml_escape("./plugins/examples");
    }
    out << "]\n";

    // [[PLUGINS.LOAD]]
    if (has(old, "PLUGINS") && has(old.at("PLUGINS"), "LOAD")) {
        for (const auto& item : old.at("PLUGINS").at("LOAD").as_array()) {
            if (!item.is_table()) continue;
            out << "[[PLUGINS.LOAD]]\n";
            if (has(item, "name"))       write_str("name",       get_str(item, "name", ""));
            if (has(item, "file"))       write_str("file",       get_str(item, "file", ""));
            if (has(item, "enabled"))    write_bool("enabled",    get_bool(item, "enabled", true));
            if (has(item, "allow_exec")) write_bool("allow_exec", get_bool(item, "allow_exec", false));
            // config 子表跳过，避免复杂迭代逻辑
            out << '\n';
        }
    }

    // 如果输出路径与旧路径相同，先备份
    if (new_path == old_path) {
        std::string bak = old_path + ".bak";
        std::ifstream src(old_path, std::ios::binary);
        if (src) {
            std::ofstream dst(bak, std::ios::binary);
            dst << src.rdbuf();
            dst.close();
            src.close();
            g_log.info("配置迁移: 已备份 %s -> %s", old_path.c_str(), bak.c_str());
        }
    }

    // 写入新配置
    std::ofstream outfile(new_path);
    if (!outfile) {
        g_log.error("配置迁移: 无法写入 %s", new_path.c_str());
        return false;
    }
    outfile << out.str();
    if (!outfile.good()) {
        g_log.error("配置迁移: 写入 %s 失败", new_path.c_str());
        return false;
    }
    outfile.close();

    g_log.info("配置迁移: 已生成 %s", new_path.c_str());
    return true;
}

// ==================== 主函数 ====================
static void print_help(const char* prog) {
    printf("high_bot 自动审核 " APP_VERSION " 开源版\n\n");
    printf("用法: %s [选项]\n\n", prog);
    printf("选项:\n");
    printf("  -h, --help      显示此帮助信息\n");
    printf("  --config <路径>  指定配置文件路径 (默认: settings.toml)\n");
    printf("  --no-tui        禁用 TUI 界面, 日志输出到终端\n");
    printf("  -once           每个家族只审核一轮即退出\n");
    printf("  --update-config [输出路径]\n");
    printf("                  读取现有配置，生成规范化新配置（默认覆盖原文件，先备份为 .bak）\n");
    printf("\n");
    printf("配置文件 settings.toml 中的可用设置:\n");
    printf("  [TUI]  ENABLED     是否启用 TUI 界面 (默认: true)\n");
    printf("  [WEB]  ENABLED     是否启用 Web 管理面板 (默认: false)\n");
    printf("  [WEB]  PORT        Web 端口 (默认: 2356)\n");
    printf("  [WEB]  BIND        Web 监听地址 (默认: 127.0.0.1)\n");
    printf("  [WEB]  ROOT        Web 静态文件目录 (默认: ./web)\n");
    printf("项目地址: https://github.com/mcpackms/ruansky_auto_review.git\n");
}

int main(int argc, char* argv[]) {
    // 解析参数
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argc > 0 ? argv[0] : "auto_review");
            return 0;
        } else if (strcmp(argv[i], "-once") == 0) {
            g_once_mode = true;
        } else if (strcmp(argv[i], "--no-tui") == 0) {
            g_no_tui = true;
        } else if (strcmp(argv[i], "--update-config") == 0) {
            std::string out_path;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                out_path = argv[++i];
            }
            curl_global_init(CURL_GLOBAL_ALL);
            g_start_time = static_cast<int>(time(nullptr));
            g_log.info("配置迁移工具");
            std::string old_path = g_config_path;
            if (out_path.empty()) out_path = old_path;
            bool ok = migrate_config(old_path, out_path);
            curl_global_cleanup();
            return ok ? 0 : 1;
        } else if (strcmp(argv[i], "--config") == 0) {
            if (i + 1 < argc) {
                g_config_path = argv[++i];
            } else {
                g_log.error("--config\t需要指定路径");
                return 1;
            }
        }
    }

    curl_global_init(CURL_GLOBAL_ALL);
    try {
        g_start_time = static_cast<int>(time(nullptr));
        g_log.info("high_bot自动审核 " APP_VERSION " 开源版");
        g_log.info("配置文件:\t%s", g_config_path.c_str());
        load_config(g_config_path);

        // CLI 参数 --no-tui 覆盖配置文件中的 [TUI] ENABLED
        if (g_no_tui) g_config.tui_enabled = false;

        load_bad_words();
        load_regex_words();

        // ===== 加载 JS 插件 =====
        try {
            auto plugin_data = toml::parse(g_config_path);
            if (plugin_data.contains("PLUGINS")) {
                bool loaded = g_plugin_mgr.load_from_config(plugin_data.at("PLUGINS"));
                if (loaded) {
                    g_log.info("✅ JS 插件已加载");
                    g_log_queue.push("[INFO]\t[PLUGIN]\t已加载");
                } else {
                    g_log.info("⚠️  JS 插件加载失败");
                    g_log_queue.push("[WARN]\t[PLUGIN]\t加载失败");
                }
            } else {
                g_log_queue.push("[INFO]\t[PLUGIN]\t未配置插件段 [PLUGINS]");
            }
        } catch (const std::exception& e) {
            g_log.error("[PLUGIN]\t解析失败:\t%s", e.what());
            g_log_queue.push("[ERROR]\t[PLUGIN]\t解析配置失败: " + std::string(e.what()));
        }

        // 注册每个家族的插件配置
        for (auto& tc : g_config.tokens) {
            for (auto& f : tc.families) {
                if (!f.plugins_dirs.empty()) {
                    g_plugin_mgr.set_family_plugin_dirs(f.family_id, f.plugins_dirs);
                }
            }
        }

        // 启动插件自动重载线程（每5分钟检测一次）
        g_plugin_mgr.start_auto_reload(300);

        std::vector<std::unique_ptr<TokenReviewer>> reviewers;
        reviewers.reserve(g_config.tokens.size());
        for (auto& tc : g_config.tokens) {
            auto r = std::make_unique<TokenReviewer>();
            r->token = tc.token;
            r->uid = tc.uid;
            r->families = tc.families;
            reviewers.push_back(std::move(r));
        }

#ifdef _WIN32
        signal(SIGINT, [](int) { g_running = false; });
        signal(SIGTERM, [](int) { g_running = false; });
#else
        struct sigaction sa;
        sa.sa_handler = [](int) { g_running = false; };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
#endif

        g_log.info("\n并发:%d\t延迟:%dms\t间隔:%ds\t正则:%s\tUP资源最大金币:%d\t主题:%s",
            g_config.concurrency, g_config.request_delay_ms,
            g_config.check_interval_seconds, g_config.enable_regex ? "开" : "关",
            g_config.max_up_resource_coin,
            g_theme == Theme::TOKYO_NIGHT ? "tokyo-night" :
            g_theme == Theme::DARK ? "dark" : "light");
        for (auto& r : reviewers) {
            g_log.info("  %s\tUID:%s", mask_token(r->token).c_str(), r->uid.c_str());
            for (auto& f : r->families) {
                int fcoin = f.max_up_resource_coin >= 0 ? f.max_up_resource_coin : g_config.max_up_resource_coin;
                g_log.info("    家族%s\t帖:%d\t评:%d\t入:%d\tUP:%d\tUP资源:%d\t最大金币:%d\t最低等级:%d",
                    f.family_id.c_str(), f.enable_post, f.enable_comment,
                    f.enable_join, f.enable_up, f.enable_up_resource, fcoin, f.min_level);
            }
        }

        // 预填充家族控制标志 (必须在启动工作线程之前完成, 避免多线程竞态)
        for (auto& r : reviewers) {
            for (auto& f : r->families) {
                if (g_family_controls.find(f.family_id) == g_family_controls.end()) {
                    g_family_controls[f.family_id] = std::make_shared<FamilyControl>();
                }
            }
        }

        // 启动 Worker 线程 (两种模式共用)
        std::vector<std::thread> threads;
        threads.reserve(reviewers.size() * 3);
        for (auto& r : reviewers) {
            for (auto& f : r->families) {
                threads.emplace_back(family_loop, r.get(), f);
            }
        }

        // 启动 Web 管理面板
        std::thread web_thread;
        if (g_config.web.enabled) {
            web_thread = std::thread([&]() {
                run_web_server(g_config.web.port, g_config.web.root, g_config.web.bind);
            });
            web_thread.detach();
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // ===== 填充家族列表（所有模式共用，Web 面板需要） =====
        for (auto& r : reviewers) {
            std::string mask = mask_token(r->token);
            for (auto& f : r->families) {
                TuiFamilyInfo info;
                info.family_id = f.family_id;
                info.token_mask = mask;
                info.uid = r->uid;
                info.post_enabled = f.enable_post && g_config.post.enabled;
                info.comment_enabled = f.enable_comment && g_config.comment.enabled;
                info.join_enabled = f.enable_join && g_config.join.enabled;
                info.up_enabled = f.enable_up && g_config.up.enabled;
                info.up_resource_enabled = f.enable_up_resource && g_config.up_resource.enabled;
                info.max_up_resource_coin = f.max_up_resource_coin >= 0
                    ? f.max_up_resource_coin
                    : g_config.max_up_resource_coin;
                info.min_level = f.min_level;
                info.control = g_family_controls[f.family_id];
                g_family_list.push_back(std::move(info));
            }
        }

        // ===== 设置统计回调（所有模式共用，使用快照保证一致性） =====
        g_get_family_stats = [&reviewers](const std::string& fid) -> TuiFamilyStats {
            TuiFamilyStats s;
            for (auto& r : reviewers) {
                for (auto& f : r->families) {
                    if (f.family_id == fid) {
                        auto ps = r->get_post_stats_snapshot(fid);
                        s.post_total = ps.total;
                        s.post_approved = ps.approved;
                        s.post_rejected = ps.rejected;
                        auto cs = r->get_comment_stats_snapshot(fid);
                        s.comment_total = cs.total;
                        s.comment_approved = cs.approved;
                        s.comment_rejected = cs.rejected;
                        auto js = r->get_join_stats_snapshot(fid);
                        s.join_total = js.total;
                        s.join_approved = js.approved;
                        s.join_rejected = js.rejected;
                        auto us = r->get_up_stats_snapshot(fid);
                        s.up_total = us.total;
                        s.up_approved = us.approved;
                        s.up_rejected = us.rejected;
                        auto urs = r->get_up_resource_stats_snapshot(fid);
                        s.up_resource_total = urs.total;
                        s.up_resource_approved = urs.approved;
                        s.up_resource_rejected = urs.rejected;
                        return s;
                    }
                }
            }
            return s;
        };

        if (g_once_mode) {
            // 一次运行模式: 等待所有线程完成即退出
            for (auto& t : threads) {
                t.join();
            }
            g_log.info("一次运行完成");
        } else if (!g_config.tui_enabled) {
            g_log.info("无TUI模式，按 Ctrl+C 停止");
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
            for (auto& t : threads) {
                t.join();
            }
            g_log.info("已停止");
        } else {
            // TUI 模式
            g_log.set_tui_mode(true);
            run_tui();
            g_log.set_tui_mode(false);

            // 通知所有线程停止并等待
            g_running = false;
            for (auto& t : threads) {
                t.join();
            }
            g_log.info("已停止");
        }
    } catch (const std::exception& e) {
        g_log.error("❌\t异常: %s", e.what());
    }
    g_plugin_mgr.stop_auto_reload();
    curl_global_cleanup();
    return 0;
}
