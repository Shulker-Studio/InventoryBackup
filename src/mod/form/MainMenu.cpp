#include "mod/form/MainMenu.h"

#include <memory>
#include <string>
#include <vector>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "mc/world/actor/player/Player.h"
#include "mod/core/BackupService.h"
#include "mod/core/BackupStore.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/core/Util.h"
#include "mod/form/Forms.h"

namespace ib {

namespace {

MenuAction backToMain() { return [](Player& who) { openMainMenu(who); }; }

std::string customField(ll::form::CustomFormResult const& result, std::string const& key) {
    auto it = result->find(key);
    if (it == result->end()) return {};
    if (auto value = std::get_if<std::string>(&it->second); value) return *value;
    return {};
}

void applySearch(
    Player&            player,
    std::string const& xboxid,
    std::string const& xuid,
    std::string const& uuid,
    MenuAction         back
) {
    auto const T = textFor(player);
    if (xboxid.empty() && xuid.empty() && uuid.empty()) {
        if (back) back(player);
        return;
    }

    auto matches = std::make_shared<std::vector<PlayerRecord>>();
    for (auto const& record : allPlayers()) {
        if (!xboxid.empty() && !containsIgnoreCase(record.name, xboxid)) continue;
        if (!xuid.empty() && !containsIgnoreCase(record.xuid, xuid)) continue;
        if (!uuid.empty() && !containsIgnoreCase(record.uuid, uuid)) continue;
        matches->push_back(record);
    }
    if (matches->empty()) {
        showNotice(player, T("backup.search.notice_title"), T("backup.search.empty"), back);
        return;
    }
    openPlayerPicker(player, T("backup.search.result"), matches, back);
}

} // namespace

void openSearchForm(Player& player, MenuAction back) {
    auto const T = textFor(player);
    ll::form::CustomForm form{T("backup.search.title")};
    form.appendInput("xboxid", T("backup.search.xboxid"));
    form.appendInput("xuid", T("backup.search.xuid"));
    form.appendInput("uuid", T("backup.search.uuid"));
    form.sendTo(player, [back, T](Player& who, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) {
            if (back) back(who);
            return;
        }
        applySearch(who, customField(result, "xboxid"), customField(result, "xuid"), customField(result, "uuid"), back);
    });
}

void openOnlinePlayers(Player& player) {
    auto const T = textFor(player);
    auto records = std::make_shared<std::vector<PlayerRecord>>();
    for (auto* online : onlinePlayers(/*includeSimulated=*/true)) {
        records->push_back(PlayerRecord{online->getUuid().asString(), online->getXuid(), online->getRealName()});
    }
    if (records->empty()) {
        showNotice(player, T("backup.menu.online"), T("backup.menu.online_empty"), backToMain());
        return;
    }
    openPlayerPicker(player, T("backup.menu.picker_online"), records, backToMain());
}

void openAllPlayers(Player& player) {
    auto const T = textFor(player);
    auto records = std::make_shared<std::vector<PlayerRecord>>(allPlayers());
    if (records->empty()) {
        showNotice(player, T("backup.menu.all"), T("backup.menu.all_empty"), backToMain());
        return;
    }
    openPlayerPicker(player, T("backup.menu.picker_all"), records, backToMain());
}

void openMainMenu(Player& player) {
    auto const T = textFor(player);
    ll::form::SimpleForm form{T("backup.menu.title")};
    form.setContent(T("backup.menu.content"));

    // 图标沿用老插件（基岩内置贴图，客户端自带，不需要额外资源包）
    form.appendButton(T("backup.menu.online"), "textures/ui/Ping_Green", "path", [T](Player& who) {
        openOnlinePlayers(who);
    });
    form.appendButton(T("backup.menu.all"), "textures/ui/icon_multiplayer", "path", [T](Player& who) {
        openAllPlayers(who);
    });
    form.appendButton(T("backup.menu.search"), "textures/ui/magnifyingGlass", "path", [T](Player& who) {
        openSearchForm(who, [T](Player& target) { openMainMenu(target); });
    });
    form.appendButton(T("backup.menu.staged"), "textures/ui/filledStar", "path", [T](Player& who) {
        openStagedMenu(who);
    });
    form.appendButton(T("backup.menu.transfer"), "textures/ui/icon_import", "path", [T](Player& who) {
        openTransferMenu(who);
    });

    // 只显示当前管理员自己的查包缓存
    if (cacheStore().has(player.getUuid().asString())) {
        form.appendButton(T("backup.menu.restore_cache"), "textures/ui/refresh_light", "path", [T](Player& who) {
            confirm(
                who,
                T("backup.menu.restore_cache.title"),
                T("backup.menu.restore_cache.content"),
                T("backup.common.confirm"),
                [T](Player& target) {
                    std::string error;
                    if (!restoreViewCache(target, &error)) {
                        tellError(target, T("backup.menu.restore_cache.failed", error));
                    } else {
                        tellOk(target, T("backup.menu.restore_cache.ok"));
                    }
                    openMainMenu(target);
                },
                [T](Player& target) { openMainMenu(target); }
            );
        });
    }

    form.sendTo(player);
}

} // namespace ib
