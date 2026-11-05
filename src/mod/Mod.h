#pragma once

#include <filesystem>
#include <optional>

#include "ll/api/io/Logger.h"
#include "ll/api/mod/NativeMod.h"
#include "mod/config/Config.h"

namespace ib {

class Mod {
public:
    static Mod& getInstance();

    Mod() : mSelf(*ll::mod::NativeMod::current()) {}

    [[nodiscard]] ll::mod::NativeMod& getSelf() const { return mSelf; }

    [[nodiscard]] std::optional<Config> const& config() const { return mConfig; }

    [[nodiscard]] std::filesystem::path dataDir() const { return getSelf().getDataDir(); }

    [[nodiscard]] std::filesystem::path configPath() const { return getSelf().getConfigDir() / "config.json"; }

    // 成功返回空串，失败返回 lang key（由调用方 msg 化后再展示）
    std::string reloadConfig();

    bool load();

    bool enable();

    bool disable();

private:
    ll::mod::NativeMod&   mSelf;
    std::optional<Config> mConfig;
};

inline ll::io::Logger& getLogger() { return Mod::getInstance().getSelf().getLogger(); }

inline Config currentConfig() { return Mod::getInstance().config().value_or(Config{}); }

} // namespace ib
