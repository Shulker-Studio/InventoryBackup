// 文件层：只负责读写、留 .bak、打日志，迁移逻辑全在 ConfigMigration.cpp
#include "mod/config/Config.h"

#include "ll/api/io/FileUtils.h"
#include "ll/api/io/Logger.h"
#include "mod/core/Util.h"
#include "mod/i18n/Text.h"

namespace ib {

std::string describeConfigError(std::string const& errorKey, std::string const& errorDetail) {
    if (errorKey.empty()) return {};
    return msg(errorKey) + errorDetail;
}

std::optional<Config> loadConfig(fs::path const& path, ll::io::Logger& logger) {
    Config defaults;

    if (!fs::exists(path)) {
        auto const text = migrateConfig("").rewritten;
        if (!ll::file_utils::writeFile(path, text)) {
            logger.error(msg("log.config.default_write_failed", formatPath(path)));
            return std::nullopt;
        }
        logger.info(msg("log.config.default_created", formatPath(path)));
        return defaults;
    }

    auto const content = ll::file_utils::readFile(path);
    if (!content) {
        logger.error(msg("log.config.read_failed", formatPath(path)));
        return std::nullopt;
    }

    auto result = migrateConfig(*content);
    if (!result.errorKey.empty()) {
        logger.error(
            msg("log.config.invalid", describeConfigError(result.errorKey, result.errorDetail), formatPath(path))
        );
        return std::nullopt;
    }

    if (result.newer) {
        logger.warn(msg("log.config.newer", result.fromVersion, kConfigVersion));
        return result.config;
    }

    if (!result.rewritten.empty()) {
        std::error_code ec;
        fs::copy_file(path, path.string() + ".bak", fs::copy_options::overwrite_existing, ec);
        if (ec) logger.warn(msg("log.config.backup_failed", ec.message()));
        if (!ll::file_utils::writeFile(path, result.rewritten)) {
            logger.error(msg("log.config.migrate_write_failed", formatPath(path)));
        } else {
            logger.info(msg("log.config.migrated", result.fromVersion, kConfigVersion, formatPath(path)));
        }
    }
    return result.config;
}

} // namespace ib
