// complete.js - 完整示例插件
//
// 演示所有 hook 和主要 API 的用法。
// 自动加载到 plugins/examples/ 目录即可启用。
//
// 配置参数（settings.toml [PLUGINS.LOAD.config]）：
//   enable_webhook: false   是否启用 Webhook 通知
//   webhook_url: ""         Webhook 目标地址
//   admin_token: ""         签名密钥（可选）

const crypto = require("crypto");

module.exports = {
    name: "complete_example",
    version: "1.0.0",
    description: "完整示例：展示所有 hooks 和 API",

    // ============================================================
    // onLoad(config)
    //
    // 插件加载时调用一次。
    // config 来自 settings.toml 的 [PLUGINS.LOAD.config] 配置节。
    // 适合在此初始化状态、恢复上次运行数据。
    // ============================================================
    onLoad(config) {
        // 读取配置
        this.enableWebhook = config.enable_webhook === true;
        this.webhookUrl = config.webhook_url || "";
        this.adminToken = config.admin_token || "";

        // 从 KV 存储恢复运行次数
        this.runCount = (kvGet("runCount") || 0) + 1;
        kvSet("runCount", this.runCount);

        // 初始化统计
        this.stats = { approved: 0, rejected: 0, rounds: 0 };
        let saved = kvGet("stats");
        if (saved) {
            this.stats.approved = saved.approved || 0;
            this.stats.rejected = saved.rejected || 0;
            this.stats.rounds = saved.rounds || 0;
        }

        log("info", "已加载，第 " + this.runCount + " 次运行");
        log("info", "历史累计: 通过 " + this.stats.approved + ", 拒绝 " + this.stats.rejected);

        // 读取元数据文件
        if (fileExists("metadata.json")) {
            let raw = readFile("metadata.json");
            if (raw) {
                try {
                    this.metadata = JSON.parse(raw);
                    log("info", "读取到元数据");
                } catch (e) {
                    log("warn", "元数据解析失败");
                }
            }
        }
    },

    // ============================================================
    // onBeforeCheck(text, familyId, type)
    //
    // 文本检查前调用。阻塞等待。
    // 返回 { reject: true, reason: "..." } 直接拒绝
    // 返回 { reject: false }              直接通过
    // 返回 null / undefined               继续默认检查
    // ============================================================
    onBeforeCheck(text, familyId, type) {
        if (text && text.length < 3 && type === "post") {
            log("info", "拒绝超短文本: family=" + familyId + " type=" + type);
            return { reject: true, reason: "文本过短" };
        }

        if (!text || text.trim().length === 0) {
            return null;
        }

        return null;
    },

    // ============================================================
    // onAfterCheck(text, result, familyId, type)
    //
    // 文本检查后调用，可以覆盖默认检查结果。
    // result: { reject: bool, reason: string }
    // 返回值同 onBeforeCheck
    // ============================================================
    onAfterCheck(text, result, familyId, type) {
        if (result && result.reject && text) {
            let lower = text.toLowerCase();
            if (lower.includes("test") || lower.includes("示例")) {
                log("info", "覆盖为放行: family=" + familyId);
                return { reject: false };
            }
        }

        return null;
    },

    // ============================================================
    // onItemApproved(familyId, type, itemId)
    // ============================================================
    onItemApproved(familyId, type, itemId) {
        this.stats.approved++;
        this._saveStats();
        this._sendWebhook("approved", { familyId, type, itemId });
    },

    // ============================================================
    // onItemRejected(familyId, type, itemId, reason)
    // ============================================================
    onItemRejected(familyId, type, itemId, reason) {
        this.stats.rejected++;
        this._saveStats();
        log("info", "拒绝: " + type + " " + itemId + " 原因: " + reason);
        this._sendWebhook("rejected", { familyId, type, itemId, reason });
    },

    // ============================================================
    // onReviewRoundStart(familyId)
    // ============================================================
    onReviewRoundStart(familyId) {
        this._roundApproved = 0;
        this._roundRejected = 0;
    },

    // ============================================================
    // onReviewRoundEnd(familyId, total, approved, rejected)
    // ============================================================
    onReviewRoundEnd(familyId, total, approved, rejected) {
        this.stats.rounds++;
        let rate = total > 0 ? ((approved / total) * 100).toFixed(1) : "N/A";
        log("info", "轮次结束: 家族 " + familyId + " " + total + " 项, 通过率 " + rate + "%");
        this._saveStats();

        if (total > 10 && rejected > approved) {
            log("warn", "拒绝率偏高 (" + rejected + "/" + total + ")");
        }

        this._generateReport(familyId);
    },

    // ============================================================
    // onError(familyId, type, errorMessage)
    // ============================================================
    onError(familyId, type, errorMessage) {
        log("error", "家族 " + familyId + " " + type + ": " + errorMessage);

        let errors = kvGet("errorLog") || [];
        errors.push({ time: timestamp(), familyId, type, message: errorMessage });
        if (errors.length > 100) errors = errors.slice(-100);
        kvSet("errorLog", errors);

        this._sendWebhook("error", { familyId, type, errorMessage });
    },

    // ============================================================
    // onPause(familyId)
    // ============================================================
    onPause(familyId) {
        log("info", "暂停: 家族 " + familyId);
        kvSet("pause_" + familyId, timestamp());
    },

    // ============================================================
    // onResume(familyId)
    // ============================================================
    onResume(familyId) {
        let t = kvGet("pause_" + familyId);
        if (t) {
            let elapsed = timestamp() - t;
            log("info", "恢复: 家族 " + familyId + " (暂停 " + elapsed + " 秒)");
            kvDel("pause_" + familyId);
        } else {
            log("info", "恢复: 家族 " + familyId);
        }
    },

    // ============================================================
    // 辅助方法
    // ============================================================

    _saveStats() {
        kvSet("stats", {
            approved: this.stats.approved,
            rejected: this.stats.rejected,
            rounds: this.stats.rounds
        });
    },

    _sendWebhook(event, data) {
        if (!this.enableWebhook || !this.webhookUrl) return;

        let payload = {
            event: event,
            plugin: "complete_example",
            timestamp: timestamp(),
            data: data
        };

        if (this.adminToken) {
            let sorted = JSON.stringify(payload);
            payload.signature = crypto.createHash("md5").update(this.adminToken + sorted).digest("hex");
        }

        // 异步发送，不等待
        httpPost(this.webhookUrl, payload, { timeout: 10 }).then(resp => {
            if (!resp.ok) {
                log("error", "Webhook 发送失败: HTTP " + resp.status);
            }
        }).catch(e => {
            log("error", "Webhook 异常: " + e.message);
        });
    },

    _generateReport(familyId) {
        let report = {
            time: timestamp(),
            plugin: "complete_example",
            runCount: this.runCount,
            stats: {
                approved: this.stats.approved,
                rejected: this.stats.rejected,
                rounds: this.stats.rounds
            },
            familyId: familyId
        };

        let filename = "report_" + timestamp() + ".json";
        let ok = writeFile(filename, JSON.stringify(report, null, 2));
        if (ok) {
            log("debug", "报告已写入: " + filename);
        }
    }
};
