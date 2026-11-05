#include <algorithm>
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

namespace ib {

std::string onlineTag(std::string const& uuid, LocaleText const& T) {
    if (onlinePlayer(uuid) != nullptr) return T("backup.player.online");
    if (hasStoredTag(uuid)) return T("backup.player.offline");
    return T("backup.player.no_save");
}

std::string playerLabel(PlayerRecord const& record, LocaleText const& T) {
    std::string tail;
    if (!record.xuid.empty()) tail += record.xuid + "/";
    tail += shortUuid(record.uuid);
    return T("backup.player.label", record.name, tail);
}

void openPlayerPicker(
    Player&                                    player,
    std::string const&                         title,
    std::shared_ptr<std::vector<PlayerRecord>> records,
    MenuAction                                 back
) {
    auto const T   = textFor(player);
    auto       items = std::make_shared<std::vector<MenuItem>>();
    items->reserve(records->size());
    for (auto const& record : *records) {
        items->push_back(MenuItem{
            playerLabel(record, T),
            [record, back, T](Player& who) { openPlayerFunc(who, record, back); }
        });
    }
    showPage(player, title, items, back);
}

void openPlayerFunc(Player& player, PlayerRecord const& record, MenuAction back) {
    auto const T = textFor(player);
    ll::form::SimpleForm form{
        "§l" + record.name,
        T("backup.player.content", record.name, onlineTag(record.uuid, T), backups().countOf(record.uuid))
    };

    form.appendButton(T("backup.player.copy"), [record, back, T](Player& who) {
        confirm(
            who,
            T("backup.player.copy.title"),
            T("backup.player.copy.content", record.name),
            T("backup.common.confirm"),
            [record, back, T](Player& target) {
                auto bag = snapshotBag(record.uuid);
                if (!bag) {
                    tellError(target, T("backup.common.no_bag"));
                    return;
                }
                std::string error;
                if (!viewBagAs(target, *bag, &error)) {
                    tellError(target, T("backup.player.copy.failed", error));
                } else {
                    tellOk(target, T("backup.player.copy.ok", record.name));
                }
                openPlayerFunc(target, record, back);
            },
            [record, back, T](Player& target) { openPlayerFunc(target, record, back); }
        );
    });

    form.appendButton(T("backup.player.overwrite"), [record, back, T](Player& who) {
        confirm(
            who,
            T("backup.player.overwrite.title"),
            T("backup.player.overwrite.content", record.name),
            T("backup.common.confirm"),
            [record, back, T](Player& target) {
                auto own = snapshotBag(target.getUuid().asString());
                if (!own) {
                    tellError(target, T("backup.common.no_own_bag"));
                    return;
                }
                if (backupPlayerSync(record.uuid, T("backup.note.pre_overwrite")) == 0) {
                    tell(target, T("backup.player.overwrite.pre_backup_failed"));
                    return;
                }
                std::string error;
                if (!restoreToPlayer(record.uuid, *own, &error)) {
                    tellError(target, T("backup.player.copy.failed", error));
                } else {
                    tellOk(target, T("backup.player.overwrite.ok", record.name));
                }
                openPlayerFunc(target, record, back);
            },
            [record, back, T](Player& target) { openPlayerFunc(target, record, back); }
        );
    });

    form.appendButton(T("backup.player.backup_now"), [record, back, T](Player& who) {
        askNoteAndBackup(who, record, back);
    });

    form.appendButton(T("backup.player.stage"), [record, back, T](Player& who) {
        auto bag = snapshotBag(record.uuid);
        if (!bag) {
            tellError(who, T("backup.common.no_bag"));
            openPlayerFunc(who, record, back);
            return;
        }
        stageBagInput(
            who,
            *bag,
            T("backup.player.stage_name", record.name),
            [record, back, T](Player& target) { openPlayerFunc(target, record, back); }
        );
    });

    form.appendButton(T("backup.player.query"), [record, back, T](Player& who) { openPlayerBackups(who, record, back); });

    form.appendButton(T("backup.player.delete"), [record, back, T](Player& who) {
        confirm(
            who,
            T("backup.player.delete.title1"),
            T("backup.player.delete.content1", record.name),
            T("backup.player.delete.confirm"),
            [record, back, T](Player& target) {
                confirm(
                    target,
                    T("backup.player.delete.title2"),
                    T("backup.player.delete.content2", record.name),
                    T("backup.player.delete.confirm2"),
                    [record, back, T](Player& who2) {
                        if (deletePlayer(record.uuid)) {
                            tellOk(who2, T("backup.player.delete.ok", record.name));
                        } else {
                            tellError(who2, T("backup.player.delete.failed"));
                        }
                        openPlayerFunc(who2, record, back);
                    },
                    [record, back, T](Player& who2) { openPlayerFunc(who2, record, back); }
                );
            },
            [record, back, T](Player& target) { openPlayerFunc(target, record, back); }
        );
    });

    if (back) form.appendButton(T("backup.common.back"), [back, T](Player& who) { back(who); });
    form.sendTo(player);
}

void askNoteAndBackup(Player& player, PlayerRecord const& record, MenuAction back) {
    auto const T = textFor(player);
    ll::form::CustomForm form{T("backup.player.backup_now.title")};
    form.appendLabel(T("backup.player.backup_now.label"));
    form.appendInput(
        "note",
        T("backup.player.backup_now.note"),
        T("backup.player.backup_now.placeholder"),
        T("backup.player.backup_now.placeholder")
    );
    form.sendTo(player, [record, back, T](Player& who, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
        if (!result) {
            openPlayerFunc(who, record, back);
            return;
        }
        std::string note = T("backup.note.manual");
        if (auto it = result->find("note"); it != result->end()) {
            if (auto value = std::get_if<std::string>(&it->second); value && !value->empty()) note = *value;
        }
        auto const time = backupPlayer(record.uuid, note);
        if (time == 0) {
            tellError(who, T("backup.player.backup_now.failed"));
        } else {
            tellOk(who, T("backup.player.backup_now.ok", record.name, formatTime(time)));
        }
        openPlayerFunc(who, record, back);
    });
}

void openPlayerBackups(Player& player, PlayerRecord const& record, MenuAction back) {
    auto const T = textFor(player);
    auto const* days = backups().daysOf(record.uuid);
    if (days == nullptr) {
        showNotice(
            player,
            T("backup.backup.query_title"),
            T("backup.backup.empty"),
            [record, back, T](Player& who) { openPlayerFunc(who, record, back); }
        );
        return;
    }

    auto items = std::make_shared<std::vector<MenuItem>>();
    for (auto const& day : *days) {
        auto const dayKey = day.day;
        auto const label  = T("backup.backup.day_label", formatDay(dayKey), day.bags.size());
        items->push_back(MenuItem{label, [record, dayKey, back, T](Player& who) {
                                    openDayBackups(who, record, dayKey, back);
                                }});
    }
    showPage(
        player,
        T("backup.backup.days_title", record.name),
        items,
        [record, back, T](Player& who) { openPlayerFunc(who, record, back); }
    );
}

void openDayBackups(Player& player, PlayerRecord const& record, int64_t day, MenuAction back) {
    auto const T = textFor(player);
    auto const* days = backups().daysOf(record.uuid);
    if (days == nullptr) {
        showNotice(player, T("backup.backup.select_title"), T("backup.backup.cleared"), back);
        return;
    }
    auto it = std::find_if(days->begin(), days->end(), [&](BackupDay const& value) { return value.day == day; });
    if (it == days->end()) {
        showNotice(player, T("backup.backup.select_title"), T("backup.backup.day_cleared"), back);
        return;
    }

    auto items = std::make_shared<std::vector<MenuItem>>();
    for (auto const& bag : it->bags) {
        auto const time  = bag.time;
        auto const label = bag.note.empty() ? "§l" + formatTime(time)
                                            : T("backup.backup.entry_label", formatTime(time), bag.note);
        items->push_back(MenuItem{label, [record, time, back, T](Player& who) {
                                    openBackupEntry(who, record, time, back);
                                }});
    }
    showPage(
        player,
        T("backup.backup.window", record.name, formatDay(day)),
        items,
        [record, back, T](Player& who) { openPlayerBackups(who, record, back); }
    );
}

void openBackupEntry(Player& player, PlayerRecord const& record, int64_t time, MenuAction back) {
    auto const T = textFor(player);
    std::string note;
    if (auto const* days = backups().daysOf(record.uuid); days != nullptr) {
        for (auto const& day : *days) {
            for (auto const& bag : day.bags) {
                if (bag.time == time) note = bag.note;
            }
        }
    }

    ll::form::SimpleForm form{
        T("backup.backup.entry_title"),
        T("backup.backup.entry_content", record.name, formatTime(time), note)
    };

    form.appendButton(T("backup.backup.view"), [record, time, back, T](Player& who) {
        auto bag = backups().read(record.uuid, time);
        if (!bag) {
            tellError(who, T("backup.common.read_backup_failed"));
            openBackupEntry(who, record, time, back);
            return;
        }
        std::string error;
        if (!viewBagAs(who, *bag, &error)) {
            tellError(who, T("backup.backup.view.failed", error));
        } else {
            tellOk(who, T("backup.backup.view.ok"));
        }
        openBackupEntry(who, record, time, back);
    });

    form.appendButton(T("backup.backup.rollback"), [record, time, back, T](Player& who) {
        confirm(
            who,
            T("backup.backup.rollback.title"),
            T("backup.backup.rollback.content", formatTime(time), record.name),
            T("backup.backup.rollback.confirm"),
            [record, time, back, T](Player& target) {
                auto bag = backups().read(record.uuid, time);
                if (!bag) {
                    tellError(target, T("backup.common.read_backup_failed"));
                    return;
                }
                if (backupPlayerSync(record.uuid, T("backup.note.pre_rollback")) == 0) {
                    tell(target, T("backup.backup.rollback.pre_backup_failed"));
                    return;
                }
                std::string error;
                if (!restoreToPlayer(record.uuid, *bag, &error)) {
                    tellError(target, T("backup.backup.rollback.failed", error));
                } else {
                    tellOk(target, T("backup.backup.rollback.ok", formatTime(time), record.name));
                }
                openBackupEntry(target, record, time, back);
            },
            [record, time, back, T](Player& target) { openBackupEntry(target, record, time, back); }
        );
    });

    form.appendButton(T("backup.backup.edit_note"), [record, time, note, back, T](Player& who) {
        ll::form::CustomForm edit{T("backup.backup.edit_note.title")};
        edit.appendLabel(T("backup.backup.edit_note.label"));
        edit.appendInput(
            "note",
            T("backup.backup.edit_note.input"),
            T("backup.backup.edit_note.placeholder"),
            note
        );
        edit.sendTo(who, [record, time, back, T](Player& target, ll::form::CustomFormResult const& result, ll::form::FormCancelReason) {
            if (!result) {
                openBackupEntry(target, record, time, back);
                return;
            }
            std::string text;
            if (auto it = result->find("note"); it != result->end()) {
                if (auto value = std::get_if<std::string>(&it->second); value) text = *value;
            }
            if (!backups().setNote(record.uuid, time, text)) {
                tellError(target, T("backup.backup.edit_note.failed"));
            } else {
                tellOk(target, T("backup.backup.edit_note.ok"));
            }
            openBackupEntry(target, record, time, back);
        });
    });

    form.appendButton(T("backup.backup.stage"), [record, time, back, T](Player& who) {
        auto bag = backups().read(record.uuid, time);
        if (!bag) {
            tellError(who, T("backup.common.read_backup_failed"));
            openBackupEntry(who, record, time, back);
            return;
        }
        stageBagInput(
            who,
            *bag,
            T("backup.backup.stage_name", record.name, formatTime(time)),
            [record, time, back, T](Player& target) { openBackupEntry(target, record, time, back); }
        );
    });

    form.appendButton(T("backup.backup.delete"), [record, time, back, T](Player& who) {
        confirm(
            who,
            T("backup.backup.delete.title"),
            T("backup.backup.delete.content", record.name, formatTime(time)),
            T("backup.backup.delete.confirm"),
            [record, time, back, T](Player& target) {
                if (backups().erase(record.uuid, time)) {
                    tellOk(target, T("backup.backup.delete.ok"));
                } else {
                    tellError(target, T("backup.backup.delete.failed"));
                }
                openPlayerBackups(target, record, back);
            },
            [record, time, back, T](Player& target) { openBackupEntry(target, record, time, back); }
        );
    });

    if (back) form.appendButton(T("backup.common.back"), [back, T](Player& who) { back(who); });
    form.sendTo(player);
}

} // namespace ib
