#include <memory>
#include <string>
#include <vector>

#include "ll/api/form/CustomForm.h"
#include "ll/api/form/SimpleForm.h"
#include "mc/world/actor/player/Player.h"
#include "mod/core/BackupService.h"
#include "mod/core/BackupStore.h"
#include "mod/core/BagOps.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/core/Util.h"
#include "mod/form/Forms.h"
#include "mod/form/MainMenu.h"

namespace ib {

namespace {

std::string customField(ll::form::CustomFormResult const& result, std::string const& key) {
    auto it = result->find(key);
    if (it == result->end()) return {};
    if (auto value = std::get_if<std::string>(&it->second); value) return *value;
    return {};
}

void openApplyStaged(Player& player, int64_t time) {
    auto const T = textFor(player);
    confirm(
        player,
        T("backup.stage.apply.title"),
        T("backup.stage.apply.content"),
        T("backup.common.confirm"),
        [time, T](Player& who) {
            auto bag = staged().read(time);
            if (!bag) {
                tellError(who, T("backup.stage.read_failed"));
                openStagedList(who);
                return;
            }
            std::string error;
            if (!restoreToPlayer(who.getUuid().asString(), *bag, &error)) {
                tellError(who, T("backup.stage.apply.failed", error));
            } else {
                tellOk(who, T("backup.stage.apply.ok"));
            }
            openStagedList(who);
        },
        [T](Player& who) { openStagedList(who); }
    );
}

void openDeleteStaged(Player& player, int64_t time, std::string const& name) {
    auto const T = textFor(player);
    (void)name;
    confirm(
        player,
        T("backup.stage.delete.title"),
        T("backup.stage.delete.content"),
        T("backup.backup.delete.confirm"),
        [time, T](Player& who) {
            if (staged().erase(time)) {
                tellOk(who, T("backup.stage.delete.ok"));
            } else {
                tellError(who, T("backup.stage.delete.failed"));
            }
            openStagedList(who);
        },
        [T](Player& who) { openStagedList(who); }
    );
}

} // namespace

void stageBagInput(Player& player, CompoundTag bag, std::string defaultName, MenuAction back) {
    auto const T = textFor(player);
    ll::form::CustomForm form{T("backup.stage.input_title")};
    form.appendLabel(T("backup.stage.input_label"));
    form.appendInput("name", T("backup.stage.input_name"), T("backup.stage.input_placeholder"), defaultName);
    form.sendTo(
        player,
        [bag = std::move(bag), back, T](Player& who, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result) {
                if (back) back(who);
                return;
            }
            auto const name = customField(result, "name");
            if (name.empty()) {
                tell(who, T("backup.stage.input_empty"));
                if (back) back(who);
                return;
            }
            if (staged().add(name, bag) == 0) {
                tellError(who, T("backup.stage.input_failed"));
            } else {
                tellOk(who, T("backup.stage.input_ok", name));
            }
            if (back) back(who);
        }
    );
}

void openStagedList(Player& player) {
    auto const T = textFor(player);
    auto const& entries = staged().entries();
    if (entries.empty()) {
        showNotice(
            player,
            T("backup.stage.list_title"),
            T("backup.stage.list_empty"),
            [T](Player& who) { openStagedMenu(who); }
        );
        return;
    }

    auto items = std::make_shared<std::vector<MenuItem>>();
    for (size_t i = 0; i < entries.size(); ++i) {
        auto const time  = entries[i].time;
        auto const name  = entries[i].name;
        auto const label = T("backup.stage.entry_label", i + 1, name, formatTime(time));
        items->push_back(MenuItem{label, [time, name, T](Player& who) {
                                    ll::form::SimpleForm form{"§l" + name, T("backup.stage.entry_content", formatTime(time))};
                                    form.appendButton(T("backup.stage.apply"), [time, T](Player& target) {
                                        openApplyStaged(target, time);
                                    });
                                    form.appendButton(T("backup.stage.delete"), [time, name, T](Player& target) {
                                        openDeleteStaged(target, time, name);
                                    });
                                    form.appendButton(T("backup.common.back"), [T](Player& target) {
                                        openStagedList(target);
                                    });
                                    form.sendTo(who);
                                }});
    }
    showPage(player, T("backup.stage.list_title"), items, [T](Player& who) { openStagedMenu(who); });
}

void openStagedMenu(Player& player) {
    auto const T = textFor(player);
    ll::form::SimpleForm form{T("backup.stage.menu_title")};
    form.setContent(T("backup.stage.menu_content"));

    form.appendButton(T("backup.stage.menu_current"), [T](Player& who) {
        auto bag = snapshotBag(who.getUuid().asString());
        if (!bag) {
            tellError(who, T("backup.common.no_own_bag"));
            openStagedMenu(who);
            return;
        }
        stageBagInput(
            who,
            *bag,
            T("backup.player.stage_name", who.getRealName()),
            [T](Player& target) { openStagedMenu(target); }
        );
    });

    form.appendButton(T("backup.stage.menu_list"), [T](Player& who) { openStagedList(who); });

    form.appendButton(T("backup.common.back"), [T](Player& who) { openMainMenu(who); });
    form.sendTo(player);
}

void openTransferMenu(Player& player) {
    auto const           T = textFor(player);
    ll::form::SimpleForm form{T("backup.transfer.menu_title")};
    form.setContent(T("backup.transfer.menu_content"));

    form.appendButton(T("backup.stage.menu_external"), "textures/ui/icon_import", "path", [T](Player& who) {
        openImportMenu(who);
    });

    form.appendButton(T("backup.stage.menu_export"), "textures/ui/store_home_icon", "path", [T](Player& who) {
        confirm(
            who,
            T("backup.stage.export_title"),
            T("backup.stage.export_content"),
            T("backup.stage.export_confirm"),
            [T](Player& target) {
                // 立刻回「开始」，之后按批处理、报进度，完成时再提示一次
                startExportAll(target.getUuid().asString(), [T](PortResult const&) {});
            },
            [T](Player& target) { openTransferMenu(target); }
        );
    });

    form.appendButton(T("backup.common.back"), [T](Player& who) { openMainMenu(who); });
    form.sendTo(player);
}

void openImportMenu(Player& player) {
    auto const T = textFor(player);
    if (!externalBagsAvailable()) {
        showNotice(
            player,
            T("backup.external.menu_title"),
            T("backup.external.missing"),
            [T](Player& who) { openTransferMenu(who); }
        );
        return;
    }

    ll::form::SimpleForm form{T("backup.external.menu_title")};
    auto const batchName = currentExportBatch().filename().string();
    form.setContent(T("backup.external.menu_content", batchName));

    form.appendButton(T("backup.external.all"), [T](Player& who) {
        auto entries = std::make_shared<std::vector<ExternalBagEntry>>(listExternalBags());
        if (entries->empty()) {
            showNotice(who, T("backup.external.all"), T("backup.external.empty"), [T](Player& target) {
                openImportMenu(target);
            });
            return;
        }
        openExternalPicker(who, entries, [T](Player& target) { openImportMenu(target); });
    });

    form.appendButton(T("backup.external.search"), [T](Player& who) {
        auto entries = std::make_shared<std::vector<ExternalBagEntry>>(listExternalBags());
        if (entries->empty()) {
            showNotice(who, T("backup.external.search"), T("backup.external.empty"), [T](Player& target) {
                openImportMenu(target);
            });
            return;
        }
        ll::form::CustomForm search{T("backup.external.search")};
        search.appendInput("xboxid", T("backup.search.xboxid"));
        search.appendInput("xuid", T("backup.search.xuid"));
        search.appendInput("uuid", T("backup.search.uuid"));
        search.sendTo(who, [entries, T](Player& target, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result) {
                openImportMenu(target);
                return;
            }
            auto const xboxid = customField(result, "xboxid");
            auto const xuid   = customField(result, "xuid");
            auto const uuid   = customField(result, "uuid");
            if (xboxid.empty() && xuid.empty() && uuid.empty()) {
                openImportMenu(target);
                return;
            }
            auto matches = std::make_shared<std::vector<ExternalBagEntry>>();
            for (auto const& entry : *entries) {
                if (!xboxid.empty() && !containsIgnoreCase(entry.name, xboxid)) continue;
                if (!xuid.empty() && !containsIgnoreCase(entry.xuid, xuid)) continue;
                if (!uuid.empty() && !containsIgnoreCase(entry.uuid, uuid)) continue;
                matches->push_back(entry);
            }
            if (matches->empty()) {
                showNotice(target, T("backup.external.search"), T("backup.external.none"), [T](Player& who2) {
                    openImportMenu(who2);
                });
                return;
            }
            openExternalPicker(target, matches, [T](Player& who2) { openImportMenu(who2); });
        });
    });

    form.appendButton(T("backup.external.import_all"), [T](Player& who) {
        confirm(
            who,
            T("backup.external.import_all_title"),
            T("backup.external.import_all_content"),
            T("backup.external.import_all_confirm"),
            [T](Player& target) {
                startImportAll(target.getUuid().asString(), [T](PortResult const&) {});
            },
            [T](Player& target) { openImportMenu(target); }
        );
    });

    form.appendButton(T("backup.common.back"), [T](Player& who) { openTransferMenu(who); });
    form.sendTo(player);
}

void openExternalPicker(
    Player&                                       player,
    std::shared_ptr<std::vector<ExternalBagEntry>> entries,
    MenuAction                                    back
) {
    auto const T   = textFor(player);
    auto       items = std::make_shared<std::vector<MenuItem>>();
    for (auto const& entry : *entries) {
        std::string const labelName = entry.name.empty() ? shortUuid(entry.uuid) : entry.name;
        std::string const labelTail =
            entry.xuid.empty() ? shortUuid(entry.uuid) : entry.xuid + "/" + shortUuid(entry.uuid);
        items->push_back(MenuItem{
            T("backup.player.label", labelName, labelTail),
            [uuid = entry.uuid, back](Player& who) { openExternalEntry(who, uuid, back); }
        });
    }
    showPage(player, T("backup.external.picker"), items, back);
}

void openExternalEntry(Player& player, std::string const& uuid, MenuAction back) {
    auto const T = textFor(player);
    auto bag = readExternalBag(uuid);
    if (!bag) {
        showNotice(player, T("backup.external.entry_title"), T("backup.external.read_failed"), back);
        return;
    }

    std::string displayName = shortUuid(uuid);
    for (auto const& entry : listExternalBags()) {
        if (entry.uuid != uuid) continue;
        if (!entry.name.empty()) displayName = entry.name;
        break;
    }

    ll::form::SimpleForm form{
        T("backup.external.entry_title"),
        T("backup.external.entry_content", displayName, describeBag(*bag))
    };

    form.appendButton(T("backup.external.to_me"), [uuid, back, T](Player& who) {
        confirm(
            who,
            T("backup.external.to_me_title"),
            T("backup.external.to_me_content"),
            T("backup.common.confirm"),
            [uuid, back, T](Player& target) {
                auto bag = readExternalBag(uuid);
                if (!bag) {
                    tellError(target, T("backup.external.read_failed_short"));
                    return;
                }
                std::string error;
                if (!viewBagAs(target, *bag, &error)) {
                    tellError(target, T("backup.player.copy.failed", error));
                } else {
                    tellOk(target, T("backup.external.to_me_ok"));
                }
                openExternalEntry(target, uuid, back);
            },
            [uuid, back, T](Player& target) { openExternalEntry(target, uuid, back); }
        );
    });

    form.appendButton(T("backup.external.to_target"), [uuid, back, T](Player& who) {
        confirm(
            who,
            T("backup.external.to_target_title"),
            T("backup.external.to_target_content"),
            T("backup.common.confirm"),
            [uuid, back, T](Player& target) {
                auto bag = readExternalBag(uuid);
                if (!bag) {
                    tellError(target, T("backup.external.read_failed_short"));
                    return;
                }
                if (backupPlayerSync(uuid, T("backup.note.pre_external")) == 0) {
                    tell(target, T("backup.player.overwrite.pre_backup_failed"));
                    return;
                }
                std::string error;
                if (!restoreToPlayer(uuid, *bag, &error)) {
                    tellError(target, T("backup.player.copy.failed", error));
                } else {
                    tellOk(target, T("backup.external.to_target_ok"));
                }
                openExternalEntry(target, uuid, back);
            },
            [uuid, back, T](Player& target) { openExternalEntry(target, uuid, back); }
        );
    });

    form.appendButton(T("backup.external.stage"), [uuid, displayName, back, T](Player& who) {
        auto bag = readExternalBag(uuid);
        if (!bag) {
            tellError(who, T("backup.external.read_failed_short"));
            openExternalEntry(who, uuid, back);
            return;
        }
        stageBagInput(
            who,
            *bag,
            T("backup.external.stage_name", displayName),
            [uuid, back, T](Player& target) { openExternalEntry(target, uuid, back); }
        );
    });

    form.appendButton(T("backup.common.back"), [back, T](Player& who) {
        if (back) back(who);
    });
    form.sendTo(player);
}

} // namespace ib
