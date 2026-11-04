// 文件层：只负责读写、留 .bak、打日志，迁移逻辑全在 ConfigMigration.cpp
#include "mod/config/Config.h"

#include "ll/api/io/FileUtils.h"
#include "ll/api/io/Logger.h"
#include "mod/core/Util.h"

namespace ib {

std::optional<Config> loadConfig(fs::path const& path, ll::io::Logger& logger) {
    Config defaults;

    if (!fs::exists(path)) {
        auto const text = migrateConfig("").rewritten;
        if (!ll::file_utils::writeFile(path, text)) {
            logger.error("无法写入默认配置: {}", path.string());
            return std::nullopt;
        }
        logger.info("已生成默认配置: {}", path.string());
        return defaults;
    }

    auto const content = ll::file_utils::readFile(path);
    if (!content) {
        logger.error("无法读取配置文件: {}", path.string());
        return std::nullopt;
    }

    auto result = migrateConfig(*content);
    if (!result.error.empty()) {
        logger.error("配置取值非法，保持上一次配置: {}（配置文件 {}）", result.error, path.string());
        return std::nullopt;
    }

    if (result.newer) {
        logger.warn(
            "配置文件版本 {} 高于插件支持的 {}，不会覆盖你的文件，只读取已知配置项",
            result.fromVersion,
            kConfigVersion
        );
        return result.config;
    }

    if (!result.rewritten.empty()) {
        std::error_code ec;
        fs::copy_file(path, path.string() + ".bak", fs::copy_options::overwrite_existing, ec);
        if (ec) logger.warn("配置文件备份失败（继续回写）: {}", ec.message());
        if (!ll::file_utils::writeFile(path, result.rewritten)) {
            logger.error("配置迁移回写失败: {}", path.string());
        } else {
            logger.info("配置已从 v{} 迁移到 v{}: {}", result.fromVersion, kConfigVersion, path.string());
        }
    }
    return result.config;
}

} // namespace ib
