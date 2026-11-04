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
    };
}

// 每一版只做自己引入的改动，用户已经写好的值一律不动
void migrateStep(int fromVersion, ojson& json) {
    switch (fromVersion) {
    case 1: // v2：新增 pageSize（表单每页条数）
        if (!json.contains("pageSize")) json["pageSize"] = Config{}.pageSize;
        break;
    default: break;
    }
}

} // namespace

std::string validateConfig(Config const& config) {
    if (config.backupIntervalMinutes < 1) return "backupIntervalMinutes 必须 >= 1";
    if (config.backupIntervalMinutes > 24 * 60) return "backupIntervalMinutes 不能超过 1440";
    if (config.maxKeepDays < 0) return "maxKeepDays 不能为负数";
    if (config.pageSize < 1) return "pageSize 必须 >= 1";
    if (config.pageSize > 50) return "pageSize 不能超过 50";
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
            result.error = std::string{"JSON 语法错误: "} + e.what();
            return result;
        } catch (std::exception const& e) {
            result.error = std::string{"配置解析失败: "} + e.what();
            return result;
        }
        if (!json.is_object()) {
            result.error = "配置根节点必须是对象";
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

    result.config = config;
    if (auto const error = validateConfig(config); !error.empty()) {
        result.error = error;
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
