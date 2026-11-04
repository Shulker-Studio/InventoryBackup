#pragma once

#include <optional>
#include <string>

#include "mc/deps/nbt/CompoundTag.h"

class Player;

namespace ib {

// 背包相关的玩家 NBT 标签（与 BDS 存档里的键名一致）
inline constexpr std::string_view kTagInventory     = "Inventory";
inline constexpr std::string_view kTagArmor         = "Armor";
inline constexpr std::string_view kTagOffhand       = "Offhand";
inline constexpr std::string_view kTagEnderChest    = "EnderChestInventory";

// 从玩家 NBT 里抽出背包部分（四个标签的浅拷贝）
std::optional<CompoundTag> extractBag(CompoundTag const& playerTag);

// 玩家当前背包快照：在线走内存，离线读世界库
std::optional<CompoundTag> snapshotBag(std::string const& uuid);

// 回档：在线立即生效（逐格写回 + 同步客户端），并写回世界库；离线直接改世界库
bool restoreBag(std::string const& uuid, CompoundTag const& bag, std::string* error = nullptr);

// 只改世界库，不动在线玩家（备份/回档内部用）
bool writeBagToStored(std::string const& uuid, CompoundTag const& bag, std::string* error = nullptr);

// 备份文件里的背包摘要，用于界面展示
std::string describeBag(CompoundTag const& bag);

} // namespace ib
