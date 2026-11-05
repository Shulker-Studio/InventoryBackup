// 配置迁移的纯逻辑：除了 nlohmann/json 不引用任何东西，
// 这样可以脱离 LeviLamina 单独编译测试（见 tests/config_test）。
#include "mod/config/Config.h"

#include <nlohmann/json.hpp>

namespace ib {

namespace {

using ojson = nlohmann::ordered_json;

ojson toJson(Config const& config) {
    return ojson{
        {"version",               kConfigVersion                },
        {"backupIntervalMinutes", config.backupIntervalMinutes  },
        {"maxKeepDays",           config.maxKeepDays            },
        {"enableIntervalBackup",  config.enableIntervalBackup   },
        {"enableLeaveBackup",     config.enableLeaveBackup      },
        {"enableDeathBackup",     config.enableDeathBackup      },
        {"skipSimulatedPlayers",  config.skipSimulatedPlayers   },
        {"pageSize",              config.pageSize               },
        {"compressLevel",         config.compressLevel          },
        {"tickBudgetMs",          config.tickBudgetMs           },
        {"portChunkSize",         config.portChunkSize          },
        {"indexCacheLimit",       config.indexCacheLimit        },
        {"backgroundIo",          config.backgroundIo           },
    };
}

// 每一版只做自己引入的改动，用户已经写好的值一律不动
void migrateStep(int fromVersion, ojson& json) {
    switch (fromVersion) {
    case 1: // v2：新增 pageSize（表单每页条数）
        if (!json.contains("pageSize")) json["pageSize"] = Config{}.pageSize;
        break;
    case 2: { // v3：新增性能旋钮
        Config const defaults;
        if (!json.contains("compressLevel")) json["compressLevel"] = defaults.compressLevel;
        if (!json.contains("tickBudgetMs")) json["tickBudgetMs"] = defaults.tickBudgetMs;
        if (!json.contains("portChunkSize")) json["portChunkSize"] = defaults.portChunkSize;
        if (!json.contains("indexCacheLimit")) json["indexCacheLimit"] = defaults.indexCacheLimit;
        if (!json.contains("backgroundIo")) json["backgroundIo"] = defaults.backgroundIo;
        break;
    }
    default: break;
    }
}

} // namespace

std::string validateConfig(Config const& config) {
    if (config.backupIntervalMinutes < 1) return "config.error.interval_min";
    if (config.backupIntervalMinutes > 24 * 60) return "config.error.interval_max";
    if (config.maxKeepDays < 0) return "config.error.keep_days_negative";
    if (config.pageSize < 1) return "config.error.page_size_min";
    if (config.pageSize > 50) return "config.error.page_size_max";
    if (!compressLevelFromName(config.compressLevel)) return "config.error.compress_level";
    if (config.tickBudgetMs < 1) return "config.error.tick_budget_min";
    if (config.tickBudgetMs > 50) return "config.error.tick_budget_max";
    if (config.portChunkSize < 1) return "config.error.port_chunk_min";
    if (config.portChunkSize > 64) return "config.error.port_chunk_max";
    if (config.indexCacheLimit < 0) return "config.error.cache_limit_negative";
    return {};
}

ConfigLoadResult migrateConfig(std::string const& text) {
    ConfigLoadResult result;
    Config           defaults;

    ojson json = ojson::object();
    if (!text.empty()) {
        try {
            json = ojson::parse(text, nullptr, /*allow_exceptions=*/true, /*ignore_comments=*/true);
        } catch (nlohmann::json::parse_error const& e) {
            result.errorKey    = "config.error.json_syntax";
            result.errorDetail = e.what();
            return result;
        } catch (std::exception const& e) {
            result.errorKey    = "config.error.parse_failed";
            result.errorDetail = e.what();
            return result;
        }
        if (!json.is_object()) {
            result.errorKey = "config.error.root_not_object";
            return result;
        }
    }

    int fileVersion = 1; // 没有 version 字段的老配置按 v1 处理
    if (auto it = json.find("version"); it != json.end() && it->is_number_integer()) {
        fileVersion = it->get<int>();
    }
    result.fromVersion = fileVersion;

    // 逐版迁移
    for (int version = fileVersion; version < kConfigVersion; ++version) {
        migrateStep(version, json);
    }

    Config config = defaults;
    auto   readInt = [&](char const* key, int& out) {
        if (auto it = json.find(key); it != json.end() && it->is_number_integer()) out = it->get<int>();
    };
    auto readBool = [&](char const* key, bool& out) {
        if (auto it = json.find(key); it != json.end() && it->is_boolean()) out = it->get<bool>();
    };

    readInt("backupIntervalMinutes", config.backupIntervalMinutes);
    readInt("maxKeepDays", config.maxKeepDays);
    readBool("enableIntervalBackup", config.enableIntervalBackup);
    readBool("enableLeaveBackup", config.enableLeaveBackup);
    readBool("enableDeathBackup", config.enableDeathBackup);
    readBool("skipSimulatedPlayers", config.skipSimulatedPlayers);
    readInt("pageSize", config.pageSize);

    if (auto it = json.find("compressLevel"); it != json.end() && it->is_string()) {
        config.compressLevel = it->get<std::string>();
    }
    readInt("tickBudgetMs", config.tickBudgetMs);
    readInt("portChunkSize", config.portChunkSize);
    readInt("indexCacheLimit", config.indexCacheLimit);
    readBool("backgroundIo", config.backgroundIo);

    result.config = config;
    if (auto const errorKey = validateConfig(config); !errorKey.empty()) {
        result.errorKey = errorKey;
        return result;
    }

    if (fileVersion > kConfigVersion) {
        // 插件比配置文件旧：不覆盖用户文件，只用自己认识的键
        result.newer = true;
        return result;
    }
    if (fileVersion < kConfigVersion) {
        // 迁移过：回写规范形状（键序统一、version 升级）
        result.rewritten = toJson(config).dump(4);
    }
    return result;
}

} // namespace ib
