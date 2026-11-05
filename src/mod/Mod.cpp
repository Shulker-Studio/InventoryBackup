#include "mod/Mod.h"

#include "ll/api/mod/RegisterHelper.h"
#include "mod/core/BackupStore.h"
#include "mod/command/Command.h"
#include "mod/events/Events.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/core/Util.h"
#include "mod/Version.h"

namespace ib {

Mod& Mod::getInstance() {
    static Mod instance;
    return instance;
}

bool Mod::load() {
    // 文案最先载入：后面每条校验日志、每个界面文案都走 i18n
    loadTexts();
    mConfig = loadConfig(configPath(), getLogger());
    if (!mConfig) {
        getLogger().error(msg("log.mod.config_load_failed", formatPath(configPath())));
        return false;
    }
    return true;
}

bool Mod::enable() {
    auto const data = dataDir();
    std::error_code ec;
    fs::create_directories(data, ec);
    if (ec) {
        getLogger().error(msg("log.mod.data_dir_create_failed", formatPath(data), ec.message()));
        return false;
    }

    loadPlayerCache(data / "players.json");
    backups().load(data);
    staged().load(data);
    cacheStore().load(data);

    registerEvents();
    startScheduler();
    registerCommand();

    getLogger().info(msg("log.mod.enabled", IB_VERSION_STRING, formatPath(data)));
    return true;
}

bool Mod::disable() {
    stopScheduler();
    unregisterEvents();
    savePlayerCache();
    getLogger().info(msg("log.mod.disabled"));
    return true;
}

std::string Mod::reloadConfig() {
    auto loaded = loadConfig(configPath(), getLogger());
    if (!loaded) return "config.error.reload_failed";
    mConfig = std::move(*loaded);
    return {};
}

} // namespace ib

LL_REGISTER_MOD(ib::Mod, ib::Mod::getInstance());
