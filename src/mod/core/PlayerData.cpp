#include "mod/core/PlayerData.h"

#include <nlohmann/json.hpp>
#include <unordered_map>

#include "ll/api/io/FileUtils.h"
#include "ll/api/io/Logger.h"
#include "ll/api/service/Bedrock.h"
#include "ll/api/service/PlayerInfo.h"
#include "mc/platform/UUID.h"
#include "mc/world/actor/player/Player.h"
#include "mc/world/level/Level.h"
#include "mc/world/actor/player/PlayerListEntry.h"
#include "mc/world/level/storage/LevelStorage.h"
#include "mc/world/level/storage/db_helpers/Category.h"
#include "mod/Mod.h"
#include "mod/core/Util.h"

namespace ib {

namespace {

using ojson = nlohmann::ordered_json;

// 实测自 Go7 世界库（tools/verify_player_key_scheme.py）：
//   player_<uuid>        -> NBT {MsaId, SelfSignedId, ServerId="player_server_<id>"}  映射记录
//   player_server_<id>   -> 玩家实体 NBT（Inventory/Armor/Offhand/EnderChestInventory…）
// 所以「数据键」必须从映射记录的 ServerId 标签里取，而不是自己拼。
constexpr std::string_view kMappingPrefix = "player_";
constexpr std::string_view kServerPrefix  = "player_server_";
constexpr std::string_view kServerIdTag   = "ServerId";

constexpr std::string_view kBagTags[] = {"Inventory", "Armor", "Offhand", "EnderChestInventory"};

LevelStorage* storage() {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    return &level->getLevelStorage();
}

// uuid -> {xuid, name}
std::unordered_map<std::string, PlayerRecord> gCache;
fs::path                                      gCachePath;
bool                                          gCacheDirty = false;

// 本次会话里已经解析过的 uuid -> 数据键（player_server_xxx）
std::unordered_map<std::string, std::string> gDataKeyCache;

std::optional<CompoundTag> gPlayerTemplate;

std::string stripUuid(std::string uuid) {
    uuid.erase(std::remove(uuid.begin(), uuid.end(), '-'), uuid.end());
    return uuid;
}

bool isPrintable(std::string_view text) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

// 映射记录里的 ServerId 已经是完整键名；兼容只有裸 id 的写法
std::string normalizeDataKey(std::string value) {
    if (value.starts_with(kServerPrefix)) return value;
    if (value.starts_with(kMappingPrefix)) return std::string{kServerPrefix} + value.substr(kMappingPrefix.size());
    return std::string{kServerPrefix} + value;
}

std::optional<std::string> serverIdFromMappingValue(std::string const& raw) {
    if (raw.empty()) return std::nullopt;
    if (auto tag = CompoundTag::fromBinaryNbt(raw, true); tag) {
        if (tag->contains(kServerIdTag, Tag::Type::String)) {
            std::string value = (*tag)[kServerIdTag];
            if (!value.empty()) return value;
        }
        return std::nullopt; // 是 NBT 但没有 ServerId：不是映射记录
    }
    if (isPrintable(raw) && raw.size() <= 64) return raw; // 旧存档里的裸字符串
    return std::nullopt;
}

} // namespace

std::optional<std::string> dataKeyOf(std::string const& uuid) {
    auto* store = storage();
    if (store == nullptr) return std::nullopt;

    if (auto it = gDataKeyCache.find(uuid); it != gDataKeyCache.end()) return it->second;

    // 1) 映射记录里的 ServerId（权威来源）
    std::string raw;
    if (store->loadData(std::string{kMappingPrefix} + uuid, raw, DBHelpers::Category::Player)) {
        if (auto sid = serverIdFromMappingValue(raw); sid) {
            auto key = normalizeDataKey(*sid);
            gDataKeyCache.emplace(uuid, key);
            return key;
        }
    }

    // 2) 在线玩家直接问 BDS（首次进服还没落盘时也能拿到正确键）
    if (auto* player = onlinePlayer(uuid); player != nullptr) {
        auto sid = store->getServerId(*player, /*isXboxLive=*/true);
        if (!sid.empty()) {
            auto key = normalizeDataKey(sid);
            gDataKeyCache.emplace(uuid, key);
            return key;
        }
    }

    // 3) 有些世界直接用 uuid 当数据键
    for (auto const& candidate : {uuid, stripUuid(uuid)}) {
        auto key = std::string{kServerPrefix} + candidate;
        if (store->hasKey(key, DBHelpers::Category::Player)) {
            gDataKeyCache.emplace(uuid, key);
            return key;
        }
    }
    return std::nullopt;
}

std::optional<CompoundTag> readStoredTag(std::string const& uuid) {
    auto* store = storage();
    if (store == nullptr) return std::nullopt;
    auto key = dataKeyOf(uuid);
    if (!key) return std::nullopt;

    std::string raw;
    if (!store->loadData(*key, raw, DBHelpers::Category::Player) || raw.empty()) return std::nullopt;
    auto tag = CompoundTag::fromBinaryNbt(raw, true);
    if (!tag) return std::nullopt;
    return std::move(*tag);
}

bool hasStoredTag(std::string const& uuid) {
    auto* store = storage();
    if (store == nullptr) return false;
    auto key = dataKeyOf(uuid);
    if (!key) return false;
    return store->hasKey(*key, DBHelpers::Category::Player);
}

bool writeStoredTag(std::string const& uuid, CompoundTag const& tag) {
    auto* store = storage();
    if (store == nullptr) return false;
    auto key = dataKeyOf(uuid);
    if (!key) return false;

    auto binary = tag.toBinaryNbt(true);
    store->saveData(*key, std::move(binary), DBHelpers::Category::Player);
    return true;
}

bool deletePlayer(std::string const& uuid) {
    auto* store = storage();
    if (store == nullptr) return false;

    auto key = dataKeyOf(uuid);
    if (!key) return false;

    // 只删实体数据键；player_<uuid> 映射保留，这样备份还能靠模板重建
    store->deleteData(*key, DBHelpers::Category::Player);
    return true;
}

std::vector<std::string> storedUuids() {
    std::vector<std::string> uuids;
    auto*                    store = storage();
    if (store == nullptr) return uuids;

    store->forEachKeyWithPrefix(
        kMappingPrefix,
        DBHelpers::Category::Player,
        [&](std::string_view key, std::string_view value) {
            std::string rawKey{key};
            // 前缀 player_ 也会命中 player_server_*，那些是数据键，不是玩家
            if (rawKey.starts_with(kServerPrefix)) return;
            if (!rawKey.starts_with(kMappingPrefix)) return;
            auto uuid = rawKey.substr(kMappingPrefix.size());
            if (uuid.empty() || uuid.starts_with("editor_")) return;
            if (auto sid = serverIdFromMappingValue(std::string{value}); sid) {
                gDataKeyCache.insert_or_assign(uuid, normalizeDataKey(*sid));
            }
            uuids.push_back(std::move(uuid));
        }
    );
    return uuids;
}

std::optional<CompoundTag> readLiveTag(Player& player) {
    CompoundTag tag;
    if (!player.save(tag)) return std::nullopt;
    if (tag.empty()) return std::nullopt;
    return tag;
}

bool hasPlayerTemplate() { return gPlayerTemplate.has_value(); }

// 从任意一份玩家 NBT 里剥出一份「空玩家模板」：去掉背包与身份标识，只留角色骨架
void capturePlayerTemplate(CompoundTag const& tag) {
    if (gPlayerTemplate) return;
    CompoundTag tmpl{tag};
    for (auto name : kBagTags) tmpl.erase(name);
    tmpl.erase("Xuid");
    tmpl.erase("UniqueID");
    tmpl.erase("ServerId");
    tmpl.erase("SelectedInventorySlot");
    if (tmpl.empty()) return;
    gPlayerTemplate = std::move(tmpl);
    getLogger().debug("已捕获空玩家模板（{} 个标签）", gPlayerTemplate->size());
}

bool createStoredTag(std::string const& uuid, CompoundTag const& templateTag, CompoundTag const& bagTag) {
    auto* store = storage();
    if (store == nullptr) return false;

    // 数据键只能来自映射记录 / BDS，拿不到就不建档
    auto key = dataKeyOf(uuid);
    if (!key) return false;

    CompoundTag merged{templateTag};
    for (auto name : kBagTags) {
        if (bagTag.contains(std::string{name})) merged[name] = bagTag[name];
    }
    auto binary = merged.toBinaryNbt(true);
    store->saveData(*key, std::move(binary), DBHelpers::Category::Player);
    return true;
}

Player* onlinePlayer(std::string const& uuid) {
    auto level = ll::service::getLevel();
    if (!level) return nullptr;
    auto const& list = level->getPlayerList();
    if (auto it = list.find(mce::UUID::fromString(uuid)); it != list.end()) {
        return level->getPlayerByXuid(it->second.mXUID);
    }
    return nullptr;
}

std::vector<Player*> onlinePlayers(bool includeSimulated) {
    std::vector<Player*> result;
    auto                 level = ll::service::getLevel();
    if (!level) return result;
    for (auto const& [uuid, entry] : level->getPlayerList()) {
        auto* player = level->getPlayerByXuid(entry.mXUID);
        if (player == nullptr) continue;
        if (!includeSimulated && player->isSimulatedPlayer()) continue;
        result.push_back(player);
    }
    return result;
}

bool isSimulated(Player const& player) { return player.isSimulatedPlayer(); }

std::optional<PlayerRecord> cachedPlayer(std::string const& uuid) {
    if (auto it = gCache.find(uuid); it != gCache.end()) return it->second;
    return std::nullopt;
}

std::vector<PlayerRecord> allPlayers() {
    std::vector<PlayerRecord> result;
    std::unordered_map<std::string, size_t> seen;

    auto push = [&](PlayerRecord record) {
        if (record.uuid.empty()) return;
        if (auto it = seen.find(record.uuid); it != seen.end()) {
            auto& existing = result[it->second];
            if (existing.name.empty()) existing.name = record.name;
            if (existing.xuid.empty()) existing.xuid = record.xuid;
            return;
        }
        seen.emplace(record.uuid, result.size());
        result.push_back(std::move(record));
    };

    for (auto const& entry : ll::service::PlayerInfo::getInstance().entries()) {
        push(PlayerRecord{entry.uuid.asString(), entry.xuid, entry.name});
    }
    for (auto const& uuid : storedUuids()) {
        PlayerRecord record{uuid, {}, {}};
        if (auto cached = cachedPlayer(uuid); cached) {
            record.xuid = cached->xuid;
            record.name = cached->name;
        }
        push(std::move(record));
    }
    for (auto const& [uuid, record] : gCache) {
        push(record);
    }
    for (auto& record : result) {
        if (record.name.empty()) record.name = shortUuid(record.uuid);
    }
    return result;
}

std::optional<PlayerRecord> findPlayer(std::string const& uuid) {
    if (auto record = cachedPlayer(uuid); record) {
        if (record->name.empty()) record->name = shortUuid(uuid);
        return record;
    }
    if (auto info = ll::service::PlayerInfo::getInstance().fromUuid(mce::UUID::fromString(uuid)); info) {
        return PlayerRecord{info->uuid.asString(), info->xuid, info->name};
    }
    return PlayerRecord{uuid, {}, shortUuid(uuid)};
}

void loadPlayerCache(fs::path const& path) {
    gCachePath = path;
    gCache.clear();
    auto const content = ll::file_utils::readFile(path);
    if (!content) return;
    try {
        auto json = ojson::parse(*content);
        for (auto it = json.begin(); it != json.end(); ++it) {
            PlayerRecord record;
            record.uuid = it.key();
            if (auto const& value = it.value(); value.is_object()) {
                record.xuid = value.value("xuid", std::string{});
                record.name = value.value("name", std::string{});
            }
            gCache.emplace(record.uuid, std::move(record));
        }
    } catch (std::exception const& e) {
        getLogger().warn("玩家名字缓存解析失败（忽略）: {}", e.what());
    }
}

void savePlayerCache() {
    if (!gCacheDirty || gCachePath.empty()) return;
    ojson json = ojson::object();
    for (auto const& [uuid, record] : gCache) {
        json[uuid] = ojson{{"xuid", record.xuid}, {"name", record.name}};
    }
    if (ll::file_utils::writeFile(gCachePath, json.dump(2))) gCacheDirty = false;
}

void rememberPlayer(std::string const& uuid, std::string const& xuid, std::string const& name) {
    if (uuid.empty()) return;
    auto& record = gCache[uuid];
    record.uuid  = uuid;
    if (!xuid.empty()) record.xuid = xuid;
    if (!name.empty()) record.name = name;
    gCacheDirty = true;
}

} // namespace ib
