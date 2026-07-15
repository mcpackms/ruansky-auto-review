// complete.js - JS 插件系统完整示例
//
// 本示例演示了所有 hooks 和主要 API 的用法。
// 可以按需启用/禁用各部分功能。
//
// 启用方法：
// 在 settings.toml 的 [PLUGINS.LOAD] 中加入：
//
//   [[PLUGINS.LOAD]]
//   name = "complete_example"
//   file = "examples/complete.js"
//   enabled = true
//   [PLUGINS.LOAD.config]
//   enable_webhook = false
//   enable_stats = true
//   webhook_url = "https://hooks.example.com/review"
//   admin_token = ""
//   enable_file_log = false

module.exports = {
    name: "complete_example",
    version: "1.0.0",
    description: "完整示例插件，展示所有 hooks 和 API",

    // ================================================================
    // onLoad(config)
    //
    // 插件加载时调用一次。config 来自 settings.toml 的
    // [PLUGINS.LOAD.config] 配置节。
    // 适合在此初始化插件状态、验证配置、恢复上次运行数据。
    // ================================================================
    onLoad(config) {
        // ---- 读取配置参数 ----
        this.enableWebhook = config.enable_webhook === true;
        this.enableStats = config.enable_stats !== false;
        this.webhookUrl = config.webhook_url || "";
        this.adminToken = config.admin_token || "";

        // ---- 使用 KV 存储恢复上次运行状态 ----
        this.runCount = (kvGet("runCount") || 0) + 1;
        kvSet("runCount", this.runCount);

        // ---- 统计计数器初始化 ----
        this.stats = {
            totalApproved: 0,
            totalRejected: 0,
            totalRounds: 0,
            roundItems: 0,
            roundApproved: 0,
            roundRejected: 0
        };

        // 读取已有统计数据
        let savedStats = kvGet("aggregatedStats");
        if (savedStats) {
            this.stats.totalApproved = savedStats.totalApproved || 0;
            this.stats.totalRejected = savedStats.totalRejected || 0;
        }

        // ---- 日志 ----
        log("info", `complete_example 加载完成，第 ${this.runCount} 次运行`);
        log("info", `历史累计: 通过 ${this.stats.totalApproved}, 拒绝 ${this.stats.totalRejected}`);

        // 使用 print 输出到 stdout（调试用）
        print("插件配置:", JSON.stringify(config));

        // ---- 演示 API 查询 ----
        let cfg = getConfig();
        log("info", `Base URL: ${cfg.base_url}`);
        log("info", `并发: ${cfg.concurrency}, 延迟: ${cfg.request_delay_ms}ms`);

        // 读取文件示例
        if (fileExists("metadata.json")) {
            let content = readFile("metadata.json");
            if (content) {
                try {
                    this.metadata = JSON.parse(content);
                    log("info", `读取到元数据文件`);
                } catch (e) {
                    log("warn", `元数据文件解析失败`);
                }
            }
        }
    },

    // ================================================================
    // onBeforeCheck(text, familyId, type)
    //
    // 文本检查前调用。
    // 可以提前放行或拒绝，避免默认检查处理。
    //
    // 返回:
    //   { reject: true, reason: "..." }   -> 直接拒绝
    //   { reject: false }                 -> 直接放行
    //   null / undefined                  -> 继续默认检查
    // ================================================================
    onBeforeCheck(text, familyId, type) {
        // 示例：检测极短文本，可能是垃圾信息
        if (text && text.length < 3 && type === "post") {
            log("info", `onBeforeCheck 拒绝超短文本: family=${familyId} type=${type}`);
            return { reject: true, reason: "complete_example: 文本过短" };
        }

        // 放行空文本（让默认检查处理）
        if (!text || text.trim().length === 0) {
            return null;
        }

        // 不做特殊处理，继续默认检查
        return null;
    },

    // ================================================================
    // onAfterCheck(text, result, familyId, type)
    //
    // 文本检查后调用，可以覆盖默认检查结果。
    //
    // result: { reject, reason }
    //   默认检查的结论（reject=true 表示应拒绝）
    //
    // 返回同 onBeforeCheck
    // ================================================================
    onAfterCheck(text, result, familyId, type) {
        // 示例：如果默认检查要拒绝，但文本包含特定关键词，则放行
        if (result && result.reject && text) {
            let lowerText = text.toLowerCase();
            if (lowerText.includes("test") || lowerText.includes("示例")) {
                log("info", `onAfterCheck 覆盖为放行: family=${familyId} type=${type}`);
                return { reject: false };
            }
        }

        return null;
    },

    // ================================================================
    // onItemApproved(familyId, type, itemId)
    //
    // 项目通过时调用（无论是由默认检查还是插件决定通过）。
    // ================================================================
    onItemApproved(familyId, type, itemId) {
        this.stats.totalApproved++;
        this.stats.roundApproved++;

        if (this.enableStats) {
            // 获取当前家族统计信息
            let stats = getStats(familyId);
            if (stats) {
                log("debug", `家族 ${familyId} 当前统计: 通过率 ${stats[type]?.rate?.toFixed(1)}%`);
            }
        }

        if (this.enableWebhook && this.webhookUrl) {
            this._sendWebhook("approved", { familyId, type, itemId });
        }
    },

    // ================================================================
    // onItemRejected(familyId, type, itemId, reason)
    //
    // 项目被拒绝时调用。
    // ================================================================
    onItemRejected(familyId, type, itemId, reason) {
        this.stats.totalRejected++;
        this.stats.roundRejected++;

        log("info", `拒绝: ${type} ${itemId} 原因: ${reason}`);

        if (this.enableWebhook && this.webhookUrl) {
            this._sendWebhook("rejected", { familyId, type, itemId, reason });
        }
    },

    // ================================================================
    // onReviewRoundStart(familyId)
    //
    // 每轮审核开始前调用。
    // ================================================================
    onReviewRoundStart(familyId) {
        this.stats.roundItems = 0;
        this.stats.roundApproved = 0;
        this.stats.roundRejected = 0;

        log("info", `=== 家族 ${familyId} 新轮次开始 ===`);
    },

    // ================================================================
    // onReviewRoundEnd(familyId, total, approved, rejected)
    //
    // 每轮审核结束后调用。
    // ================================================================
    onReviewRoundEnd(familyId, total, approved, rejected) {
        this.stats.totalRounds++;
        this.stats.roundItems = total;

        let passRate = total > 0 ? ((approved / total) * 100).toFixed(1) : "N/A";

        log("info", `=== 家族 ${familyId} 轮次结束: ${total} 项, 通过 ${approved}, 拒绝 ${rejected}, 通过率 ${passRate}% ===`);

        // 保存聚合统计数据到 KV 存储
        kvSet("aggregatedStats", {
            totalApproved: this.stats.totalApproved,
            totalRejected: this.stats.totalRejected,
            totalRounds: this.stats.totalRounds,
            lastRoundTime: timestamp()
        });

        // 示例：如果拒绝率过高，输出警告
        if (total > 10 && rejected > approved) {
            log("warn", `家族 ${familyId} 拒绝率偏高 (${rejected}/${total})，建议检查规则配置`);
        }
    },

    // ================================================================
    // onError(familyId, type, errorMessage)
    //
    // 发生错误时调用。
    // ================================================================
    onError(familyId, type, errorMessage) {
        log("error", `[${type}] 家族 ${familyId}: ${errorMessage}`);

        // 记录错误到文件
        try {
            let errors = kvGet("errorLog") || [];
            errors.push({
                time: timestamp(),
                familyId: familyId,
                type: type,
                message: errorMessage
            });
            // 保留最近 100 条错误记录
            if (errors.length > 100) {
                errors = errors.slice(-100);
            }
            kvSet("errorLog", errors);
        } catch (e) {
            // 避免递归错误
        }

        if (this.enableWebhook && this.webhookUrl) {
            this._sendWebhook("error", { familyId, type, errorMessage });
        }
    },

    // ================================================================
    // onPause(familyId)
    //
    // 家族审核暂停时调用。
    // ================================================================
    onPause(familyId) {
        log("info", `家族 ${familyId} 暂停`);

        // 保存暂停时间
        kvSet("pauseTime_" + familyId, timestamp());
    },

    // ================================================================
    // onResume(familyId)
    //
    // 家族审核恢复时调用。
    // ================================================================
    onResume(familyId) {
        let pauseTime = kvGet("pauseTime_" + familyId);
        if (pauseTime) {
            let duration = timestamp() - pauseTime;
            log("info", `家族 ${familyId} 恢复 (暂停 ${duration} 秒)`);
            kvDel("pauseTime_" + familyId);
        } else {
            log("info", `家族 ${familyId} 恢复`);
        }
    },

    // ================================================================
    // 辅助方法
    // ================================================================

    // 发送 Webhook 通知
    _sendWebhook(event, data) {
        try {
            let payload = {
                event: event,
                plugin: "complete_example",
                timestamp: timestamp(),
                data: data
            };

            // 如果配置了 admin_token，计算签名
            if (this.adminToken) {
                let payloadStr = JSON.stringify(payload);
                payload.signature = md5(this.adminToken + payloadStr);
            }

            let resp = httpPost(this.webhookUrl, payload, { timeout: 10 });

            if (!resp.ok) {
                log("error", `Webhook 发送失败: HTTP ${resp.status}`);
            }
        } catch (e) {
            log("error", `Webhook 异常: ${e.toString()}`);
        }
    },

    // ================================================================
    // 插件内部定时报告（通过轮次结束 hook 触发）
    // ================================================================

    // 生成运行报告并写入文件
    _generateReport() {
        let report = {
            plugin: "complete_example",
            generatedAt: timestamp(),
            runCount: this.runCount,
            stats: {
                totalApproved: this.stats.totalApproved,
                totalRejected: this.stats.totalRejected,
                totalRounds: this.stats.totalRounds
            },
            families: []
        };

        // 获取所有家族统计
        let familyCount = getFamilyCount();
        for (let i = 0; i < familyCount; i++) {
            let fam = getFamilyConfig(0, i);
            if (fam) {
                let famStats = getStats(fam.family_id);
                if (famStats) {
                    report.families.push({
                        familyId: fam.family_id,
                        stats: famStats
                    });
                }
            }
        }

        let reportStr = JSON.stringify(report, null, 2);
        let filename = "report_" + Math.floor(timestamp()) + ".json";
        let ok = writeFile(filename, reportStr);
        if (ok) {
            log("info", `报告已写入: ${filename}`);
        } else {
            log("error", `报告写入失败: ${filename}`);
        }
    }
};
