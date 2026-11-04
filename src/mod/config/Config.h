#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ll::io {
class Logger;
}

namespace ib {

// 当前配置结构版本。改动配置形状时：+1，并在 ConfigMigration.cpp 的迁移表里加一步
constexpr int kConfigVersion = 2;

// 配置形状就是这一版配置的全部内容，缺项按默认值补
struct Config {
    int  backupIntervalMinutes = 15;   // 在线定时备份间隔（分钟）
    int  maxKeepDays           = 90;   // 备份最多保留多少天，0 表示永不清理
    bool enableIntervalBackup  = true; // 在线定时备份
    bool enableLeaveBackup     = true; // 玩家退出时备份
    bool enableDeathBackup     = false;// 玩家死亡时备份（死亡掉落服用）
    bool skipSimulatedPlayers  = true; // 跳过模拟玩家（NPC）
    int  pageSize              = 10;   // 表单每页显示多少条（1-50）
};

// 纯逻辑的加载结果（不碰文件系统，方便离线测试）
struct ConfigLoadResult {
    Config      config;
    std::string rewritten; // 非空表示需要回写到磁盘的完整配置
    std::string error;     // 非空表示拒绝加载（保留上一份配置）
    int         fromVersion = kConfigVersion;
    bool        newer       = false; // 文件版本比插件新
};

// 解析 + 按 version 逐版迁移 + 取值校验。空文本视作全新配置
ConfigLoadResult migrateConfig(std::string const& text);

// 文件层：读写、迁移、写回前留 .bak、日志
std::optional<Config> loadConfig(std::filesystem::path const& path, ll::io::Logger& logger);

// 供 /inventorybackup reload 做取值校验
std::string validateConfig(Config const& config);

} // namespace ib
