#pragma once

#include <string>
#include <string_view>

#include "ll/api/i18n/I18n.h"
#include "mc/world/actor/player/Player.h"

namespace ib {

// 载入 lang/ 下的语言文件；失败只告警（此时界面会显示 key 本身，便于发现漏翻）
void loadTexts();

// 按默认语言取文案并填充 {} 参数
template <class... Args>
std::string msg(std::string_view key, Args&&... args) {
    auto pattern = ll::i18n::getInstance().get(key, {});
    if (pattern.empty()) return std::string{key};
    if constexpr (sizeof...(Args) == 0) {
        return std::string{pattern};
    } else {
        return fmt::vformat(pattern, fmt::make_format_args(args...));
    }
}

// 按指定语言取文案
template <class... Args>
std::string msgFor(std::string_view localeCode, std::string_view key, Args&&... args) {
    auto pattern = ll::i18n::getInstance().get(key, localeCode);
    if (pattern.empty()) return std::string{key};
    if constexpr (sizeof...(Args) == 0) {
        return std::string{pattern};
    } else {
        return fmt::vformat(pattern, fmt::make_format_args(args...));
    }
}

// 绑定某位玩家的语言：界面文案跟随执行命令的玩家，而不是服务器默认语言
struct LocaleText {
    std::string locale;

    template <class... Args>
    std::string operator()(std::string_view key, Args&&... args) const {
        return msgFor(locale, key, std::forward<Args>(args)...);
    }
};

inline LocaleText textFor(Player const& player) { return LocaleText{player.getLocaleCode()}; }

} // namespace ib
