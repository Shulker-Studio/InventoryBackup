#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace ll::io {
class Logger;
}

namespace ib {

// 当前配置结构版本。改动配置形状时：+1，并在 ConfigMigration.cpp 的迁移表里加一步
constexpr int kConfigVersion = 3;

// 压缩级别。合法取值只在这一张表里定义，配置校验和备份写盘都从它取：
// 将来加档位只改这里，不会出现「校验放行、运行时静默落回默认」的漂移
enum class CompressLevel { Fastest, Default, Best };

struct CompressLevelName {
    std::string_view name;
    CompressLevel    level;
};

inline constexpr CompressLevelName kCompressLevels[] = {
    {"fastest", CompressLevel::Fastest},
    {"default", CompressLevel::Default},
    {"best",    CompressLevel::Best   }
};

inline constexpr std::string_view kDefaultCompressLevelName = "default";

// 名字 -> 档位，认不出来返回 nullopt
constexpr std::optional<CompressLevel> compressLevelFromName(std::string_view name) {
    for (auto const& entry : kCompressLevels) {
        if (entry.name == name) return entry.level;
    }
    return std::nullopt;
}

// 配置形状就是这一版配置的全部内容，缺项按默认值补
struct Config {
    int  backupIntervalMinutes = 15;   // 在线定时备份间隔（分钟）
    int  maxKeepDays           = 90;   // 备份最多保留多少天，0 表示永不清理
    bool enableIntervalBackup  = true; // 在线定时备份
    bool enableLeaveBackup     = true; // 玩家退出时备份
    bool enableDeathBackup     = false;// 玩家死亡时备份（死亡掉落服用）
    bool skipSimulatedPlayers  = true; // 跳过模拟玩家（NPC）
    int  pageSize              = 10;   // 表单每页显示多少条（1-50）

    // 性能相关（都是一边省 CPU/内存、一边多占磁盘/多读盘的旋钮）
    std::string compressLevel = std::string{kDefaultCompressLevelName}; // 压缩级别，取值见 kCompressLevels
    int         tickBudgetMs    = 5;         // 备份、清理各自每 tick 最多花多少毫秒（1-50）
    int         portChunkSize   = 2;         // 导入导出每批处理多少玩家（1-64）
    int         indexCacheLimit = 32;        // 内存里最多缓存多少个玩家的备份索引，0 表示不限
    bool        backgroundIo    = true;      // 压缩/读写盘放线程池，不占服务端线程
};

// 纯逻辑的加载结果（不碰文件系统、不碰 i18n，方便离线测试）
struct ConfigLoadResult {
    Config      config;
    std::string rewritten; // 非空表示需要回写到磁盘的完整配置
    std::string errorKey;    // 非空表示拒绝加载（保留上一份配置），值为 lang key
    std::string errorDetail; // 附在错误文案后面的细节（如 JSON 解析器的 what()）
    int         fromVersion = kConfigVersion;
    bool        newer       = false; // 文件版本比插件新
};

// 解析 + 按 version 逐版迁移 + 取值校验。空文本视作全新配置
ConfigLoadResult migrateConfig(std::string const& text);

// 文件层：读写、迁移、写回前留 .bak、日志
std::optional<Config> loadConfig(std::filesystem::path const& path, ll::io::Logger& logger);

// 供 /inventorybackup reload 做取值校验；通过返回空串，否则返回 lang key
std::string validateConfig(Config const& config);

// 把 errorKey + errorDetail 拼成可直接展示/记日志的文本（key 为空返回空串）
std::string describeConfigError(std::string const& errorKey, std::string const& errorDetail);

} // namespace ib
