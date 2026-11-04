#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "mc/deps/nbt/CompoundTag.h"

class Player;

namespace ib {

struct PlayerRecord {
    std::string uuid;
    std::string xuid;
    std::string name; // 可能是空（只在世界库里有存档、又没有名字缓存时）
};

// 合并「世界库里的存档 uuid」「BDS 玩家信息缓存」「本模组自己的名字缓存」，去重
std::vector<PlayerRecord> allPlayers();

std::optional<PlayerRecord> findPlayer(std::string const& uuid);

Player* onlinePlayer(std::string const& uuid);

// 当前在线玩家（已过滤模拟玩家，除非 includeSimulated）
std::vector<Player*> onlinePlayers(bool includeSimulated = false);

bool isSimulated(Player const& player);

// ---------------- 世界库（LevelDB）里的玩家 NBT ----------------
// BDS 实际用的两条键（已用真实世界库验证，见 tools/verify_player_key_scheme.py）：
//   player_<uuid>       -> 映射记录 {MsaId, SelfSignedId, ServerId="player_server_<id>"}
//   player_server_<id>  -> 玩家实体 NBT（含 Inventory/Armor/Offhand/EnderChestInventory）
// 全部走 LevelStorage，不依赖 GMLIB。

// uuid -> 实体数据键（player_server_<id>，从映射记录的 ServerId 里取）
std::optional<std::string> dataKeyOf(std::string const& uuid);

std::optional<CompoundTag> readStoredTag(std::string const& uuid);

bool hasStoredTag(std::string const& uuid);

bool writeStoredTag(std::string const& uuid, CompoundTag const& tag);

// 删除世界库里的玩家实体数据（只删 player_server_<id>，映射记录保留）
bool deletePlayer(std::string const& uuid);

// 世界库里所有有玩家数据键的 uuid
std::vector<std::string> storedUuids();

// 在线玩家当前状态的 NBT 快照（走 Actor::save，和存档里的玩家 NBT 同一套标签）
std::optional<CompoundTag> readLiveTag(Player& player);

// 新建玩家存档（玩家从没进过服 / 被删除过时的回档）：需要模板 NBT
bool hasPlayerTemplate();
void capturePlayerTemplate(CompoundTag const& tag);
bool createStoredTag(std::string const& uuid, CompoundTag const& templateTag, CompoundTag const& bagTag);

// ---------------- 名字 / xuid 缓存 ----------------
void loadPlayerCache(std::filesystem::path const& path);

void savePlayerCache();

void rememberPlayer(std::string const& uuid, std::string const& xuid, std::string const& name);

std::optional<PlayerRecord> cachedPlayer(std::string const& uuid);

} // namespace ib
