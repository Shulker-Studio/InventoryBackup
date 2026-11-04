#include "mod/core/BagOps.h"

#include <array>
#include <vector>

#include "ll/api/service/Bedrock.h"
#include "mc/deps/nbt/ListTag.h"
#include "mc/deps/shared_types/legacy/actor/ArmorSlot.h"
#include "mc/world/Container.h"
#include "mc/world/actor/player/Inventory.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/inventory/EnderChestContainer.h"
#include "mc/world/item/ItemStack.h"
#include "mc/world/level/BlockPos.h"
#include "mc/world/level/Level.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/core/Util.h"

namespace ib {

namespace {

using enum SharedTypes::Legacy::ArmorSlot;

constexpr std::array<std::string_view, 4> kBagTags{kTagInventory, kTagArmor, kTagOffhand, kTagEnderChest};

// 存档里的物品项自带 Slot（Byte 或 Int），列表只装非空物品，所以不能拿数组下标当槽位
std::optional<int> itemSlot(CompoundTag const& item) {
    if (item.contains("Slot", Tag::Type::Byte)) return static_cast<int>(item["Slot"].get<int>());
    if (item.contains("Slot", Tag::Type::Int)) return item["Slot"].get<int>();
    return std::nullopt;
}

bool listHasSlots(ListTag const& list) {
    for (auto const& element : list) {
        if (element.hold<CompoundTag>() && itemSlot(element.get<CompoundTag>())) return true;
    }
    return false;
}

// 逐项落到容器里：有 Slot 就按 Slot 落，老格式（整表没 Slot）才退回下标顺序
template <class SetFn>
void placeList(ListTag const& list, int containerSize, bool useSlots, SetFn&& set) {
    int fallback = 0;
    for (auto const& element : list) {
        if (!element.hold<CompoundTag>()) continue;
        auto const& item = element.get<CompoundTag>();

        int slot = fallback;
        if (useSlots) {
            auto const stored = itemSlot(item);
            if (!stored) {
                // 有 Slot 的表里出现无 Slot 的项：不猜位置，跳过
                continue;
            }
            slot = *stored;
        }
        ++fallback;
        if (slot < 0 || slot >= containerSize) continue;
        set(slot, item);
    }
}

void clearOnlineBag(Player& player) {
    auto& inventory = player.getInventory();
    for (int slot = 0; slot < inventory.getContainerSize(); ++slot) {
        inventory.setItem(slot, ItemStack::EMPTY_ITEM());
    }
    for (auto slot : {Head, Torso, Legs, Feet}) player.setArmor(slot, ItemStack::EMPTY_ITEM());
    player.setOffhandSlot(ItemStack::EMPTY_ITEM());
    if (auto enderChest = player.getEnderChestContainer(); enderChest) {
        for (int slot = 0; slot < enderChest->getContainerSize(); ++slot) {
            enderChest->setItem(slot, ItemStack::EMPTY_ITEM());
        }
    }
}

void applyListToContainer(Container& container, CompoundTag const& bag, std::string_view key, ILevel& level) {
    if (!bag.contains(key, Tag::Type::List)) return;
    auto const& list = bag[key].get<ListTag>();
    placeList(list, container.getContainerSize(), listHasSlots(list), [&](int slot, CompoundTag const& item) {
        container.setItem(slot, ItemStack::fromTag(item, level));
    });
}

void applyArmor(Player& player, CompoundTag const& bag, ILevel& level) {
    if (!bag.contains(kTagArmor, Tag::Type::List)) return;
    auto const&                                        list  = bag[kTagArmor].get<ListTag>();
    constexpr std::array<SharedTypes::Legacy::ArmorSlot, 4> slots{Head, Torso, Legs, Feet};
    placeList(list, static_cast<int>(slots.size()), listHasSlots(list), [&](int slot, CompoundTag const& item) {
        player.setArmor(slots[static_cast<size_t>(slot)], ItemStack::fromTag(item, level));
    });
}

bool applyBagToOnline(Player& player, CompoundTag const& bag) {
    auto level = ll::service::getLevel();
    if (!level) return false;

    clearOnlineBag(player);
    applyListToContainer(player.getInventory(), bag, kTagInventory, *level);
    applyArmor(player, bag, *level);
    if (bag.contains(kTagOffhand, Tag::Type::Compound)) {
        player.setOffhandSlot(ItemStack::fromTag(bag[kTagOffhand].get<CompoundTag>(), *level));
    }
    if (auto enderChest = player.getEnderChestContainer(); enderChest) {
        applyListToContainer(*enderChest, bag, kTagEnderChest, *level);
    }
    player.sendInventory(true);
    return true;
}

void replaceBagTags(CompoundTag& target, CompoundTag const& bag) {
    for (auto tag : kBagTags) {
        if (bag.contains(std::string{tag})) {
            target[tag] = bag[tag];
        } else {
            target.erase(tag);
        }
    }
}

// 玩家数据键被删过时，用模板 NBT + 现有 serverId 建档（serverId 只从 BDS 已有映射里取，绝不凭空生成）
std::optional<CompoundTag> buildNewPlayerTag(CompoundTag const& bag) {
    auto level = ll::service::getLevel();
    if (!level) return std::nullopt;

    auto        spawn = level->getDefaultSpawn();
    CompoundTag tag;
    // 模板是「别人」的 NBT：位置/朝向/维度必须重置，否则新档会落在别人的坐标上
    tag["Pos"]      = ListTag{std::vector<CompoundTagVariant>{spawn.x + 0.5, spawn.y + 1.0, spawn.z + 0.5}};
    tag["Rotation"] = ListTag{std::vector<CompoundTagVariant>{0.0f, 0.0f}};
    tag.putInt("DimensionId", 0);
    tag.putInt("SpawnDimension", 0);
    tag.putInt("SpawnX", spawn.x);
    tag.putInt("SpawnY", spawn.y);
    tag.putInt("SpawnZ", spawn.z);
    tag.putInt("SpawnBlockPositionX", spawn.x);
    tag.putInt("SpawnBlockPositionY", spawn.y);
    tag.putInt("SpawnBlockPositionZ", spawn.z);
    replaceBagTags(tag, bag);
    return tag;
}

} // namespace

std::optional<CompoundTag> extractBag(CompoundTag const& playerTag) {
    CompoundTag bag;
    for (auto tag : kBagTags) {
        if (playerTag.contains(std::string{tag})) bag[tag] = playerTag[tag];
    }
    if (bag.empty()) return std::nullopt;
    return bag;
}

std::optional<CompoundTag> snapshotBag(std::string const& uuid) {
    if (auto* player = onlinePlayer(uuid); player != nullptr) {
        if (auto live = readLiveTag(*player); live) {
            if (auto bag = extractBag(*live); bag) return bag;
        }
    }
    auto stored = readStoredTag(uuid);
    if (!stored) return std::nullopt;
    return extractBag(*stored);
}

bool writeBagToStored(std::string const& uuid, CompoundTag const& bag, std::string* error) {
    auto stored = readStoredTag(uuid);
    if (stored) {
        replaceBagTags(*stored, bag);
        if (!writeStoredTag(uuid, *stored)) {
            if (error) *error = msg("backup.error.write_failed");
            return false;
        }
        return true;
    }

    if (!dataKeyOf(uuid)) {
        // 找不到映射记录 / BDS 给不出数据键：不猜，直接拒绝
        if (error) *error = msg("backup.error.no_server_id");
        return false;
    }
    if (!hasPlayerTemplate()) {
        if (error) *error = msg("backup.error.no_template");
        return false;
    }
    auto built = buildNewPlayerTag(bag);
    if (!built) {
        if (error) *error = msg("backup.common.world_not_loaded");
        return false;
    }
    if (!createStoredTag(uuid, *built, bag)) {
        if (error) *error = msg("backup.error.write_failed");
        return false;
    }
    return true;
}

bool restoreBag(std::string const& uuid, CompoundTag const& bag, std::string* error) {
    if (auto* player = onlinePlayer(uuid); player != nullptr) {
        if (!applyBagToOnline(*player, bag)) {
            if (error) *error = msg("backup.common.world_not_loaded");
            return false;
        }
        // 内存里已经生效，再同步一份到世界库，避免崩溃后不一致
        if (auto live = readLiveTag(*player); live) {
            replaceBagTags(*live, bag);
            writeStoredTag(uuid, *live);
        }
        return true;
    }
    return writeBagToStored(uuid, bag, error);
}

std::string describeBag(CompoundTag const& bag) {
    int64_t                  total = 0;
    std::vector<std::string> names;

    auto scanList = [&](std::string_view key) {
        if (!bag.contains(key, Tag::Type::List)) return;
        for (auto const& element : bag[key].get<ListTag>()) {
            if (!element.hold<CompoundTag>()) continue;
            auto const& item  = element.get<CompoundTag>();
            int         count = 1;
            if (item.contains("Count", Tag::Type::Int)) count = item["Count"].get<int>();
            if (count <= 0) continue;
            total += count;
            if (item.contains("Name", Tag::Type::String)) {
                std::string name = item["Name"];
                if (std::find(names.begin(), names.end(), name) == names.end() && names.size() < 8) {
                    names.push_back(std::move(name));
                }
            }
        }
    };

    scanList(kTagInventory);
    scanList(kTagArmor);
    scanList(kTagEnderChest);
    if (bag.contains(kTagOffhand, Tag::Type::Compound)) {
        auto const& item  = bag[kTagOffhand].get<CompoundTag>();
        int         count = 1;
        if (item.contains("Count", Tag::Type::Int)) count = item["Count"].get<int>();
        if (count > 0) {
            total += count;
            if (item.contains("Name", Tag::Type::String)) {
                std::string name = item["Name"];
                if (std::find(names.begin(), names.end(), name) == names.end() && names.size() < 8) {
                    names.push_back(std::move(name));
                }
            }
        }
    }

    if (total <= 0) return msg("backup.bag.empty");
    std::string summary = msg("backup.bag.summary", total);
    if (!names.empty()) {
        summary += msg("backup.bag.summary_names") + names[0];
        for (size_t i = 1; i < names.size(); ++i) {
            summary += msg("backup.bag.summary_separator") + names[i];
        }
        if (names.size() == 8) summary += msg("backup.bag.summary_more");
    }
    return summary;
}

} // namespace ib
