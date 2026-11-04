#include "mod/Mod.h"

#include "ll/api/mod/RegisterHelper.h"
#include "mod/core/BackupStore.h"
#include "mod/command/Command.h"
#include "mod/events/Events.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/core/Util.h"

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
        getLogger().error("配置加载失败，模组不会启用（请修正 {} 后重启或 /inventorybackup reload）", configPath().string());
        return false;
    }
    return true;
}

bool Mod::enable() {
    auto const data = dataDir();
    std::error_code ec;
    fs::create_directories(data, ec);
    if (ec) {
        getLogger().error("无法创建数据目录 {}: {}", data.string(), ec.message());
        return false;
    }

    loadPlayerCache(data / "players.json");
    backups().load(data);
    staged().load(data);
    cacheStore().load(data);

    registerEvents();
    startScheduler();
    registerCommand();

    getLogger().info("背包备份已启用，数据目录: {}", data.string());
    return true;
}

bool Mod::disable() {
    stopScheduler();
    unregisterEvents();
    savePlayerCache();
    getLogger().info("背包备份已停用");
    return true;
}

std::string Mod::reloadConfig() {
    auto loaded = loadConfig(configPath(), getLogger());
    if (!loaded) return "配置解析失败，保持原配置（详见日志）";
    mConfig = std::move(*loaded);
    return {};
}

} // namespace ib

LL_REGISTER_MOD(ib::Mod, ib::Mod::getInstance());
