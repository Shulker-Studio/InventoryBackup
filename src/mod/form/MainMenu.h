#pragma once

class Player;

namespace ib {

// 主菜单（玩家执行 /inventorybackup 打开）
void openMainMenu(Player& player);

// 命令与快捷入口
void openOnlinePlayers(Player& player);

void openAllPlayers(Player& player);

} // namespace ib
