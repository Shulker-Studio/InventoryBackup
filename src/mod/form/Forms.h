#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mod/core/BackupService.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/form/FormCommon.h"

class Player;

namespace ib {

// ---------------------------------------------------------------- 玩家
void openPlayerPicker(
    Player&                                     player,
    std::string const&                          title,
    std::shared_ptr<std::vector<PlayerRecord>>  records,
    MenuAction                                  back
);

void openSearchForm(Player& player, MenuAction back);

void openPlayerFunc(Player& player, PlayerRecord const& record, MenuAction back);

// 列表 / 详情用的文案
std::string onlineTag(std::string const& uuid, LocaleText const& T);

std::string playerLabel(PlayerRecord const& record, LocaleText const& T);

// ---------------------------------------------------------------- 备份
void openPlayerBackups(Player& player, PlayerRecord const& record, MenuAction back);

void openDayBackups(Player& player, PlayerRecord const& record, int64_t day, MenuAction back);

void openBackupEntry(Player& player, PlayerRecord const& record, int64_t time, MenuAction back);

void askNoteAndBackup(Player& player, PlayerRecord const& record, MenuAction back);

// ---------------------------------------------------------------- 暂存
void stageBagInput(Player& player, CompoundTag bag, std::string defaultName, MenuAction back);

void openStagedMenu(Player& player);

void openTransferMenu(Player& player);

void openStagedList(Player& player);

// ---------------------------------------------------------------- 外部背包
void openImportMenu(Player& player);

void openExternalPicker(
    Player&                                        player,
    std::shared_ptr<std::vector<ExternalBagEntry>> entries,
    MenuAction                                     back
);

void openExternalEntry(Player& player, std::string const& uuid, MenuAction back);

} // namespace ib
