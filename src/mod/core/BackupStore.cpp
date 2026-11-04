#include "mod/core/BackupStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <system_error>

#include "ll/api/io/FileUtils.h"
#include "ll/api/io/Logger.h"
#include "ll/api/utils/CompressUtils.h"
#include "mod/Mod.h"
#include "mod/core/Util.h"

namespace ib {

namespace {

using ojson = nlohmann::ordered_json;

constexpr char kBagFileSuffix[]  = ".nbt.gz";
constexpr char kIndexFileName[]  = "index.json";
constexpr char kStoreFileName[]  = "store.json";
constexpr char kMetaFileName[]   = "meta.json";
constexpr char kCacheBagName[]   = "bag.nbt.gz";

void ensureDirectory(fs::path const& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
}

// 先写 .tmp 再改名，避免写一半崩服留下坏索引
bool writeFileAtomic(fs::path const& path, std::string const& content) {
    auto const tmp = path.string() + ".tmp";
    if (!ll::file_utils::writeFile(tmp, content)) return false;
    std::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    return true;
}

std::string brokenName(fs::path const& path) { return path.string() + ".broken-" + std::to_string(nowMs()); }

void renameAside(fs::path const& path, std::string const& target, char const* what) {
    std::error_code ec;
    fs::rename(path, target, ec);
    if (ec) {
        getLogger().warn("{} 改名失败（{}）: {}", what, ec.message(), path.string());
    } else {
        getLogger().warn("{} 已改名为 {}，原内容不再参与加载", what, target);
    }
}

// 读全局 players 索引（老布局 / 迁移用）
std::optional<ojson> parseJsonFile(fs::path const& path) {
    auto const content = ll::file_utils::readFile(path);
    if (!content) return std::nullopt;
    try {
        return ojson::parse(*content);
    } catch (std::exception const& e) {
        getLogger().error("JSON 解析失败（{}）: {}", path.string(), e.what());
        return std::nullopt;
    }
}

std::vector<BackupDay> parseDays(ojson const& dayArray) {
    std::vector<BackupDay> days;
    for (auto const& dayJson : dayArray) {
        BackupDay day;
        day.day = dayJson.value("day", int64_t{0});
        for (auto const& bagJson : dayJson.value("bags", ojson::array())) {
            BackupEntry entry;
            entry.time = bagJson.value("time", int64_t{0});
            entry.note = bagJson.value("note", std::string{});
            if (entry.time != 0) day.bags.push_back(std::move(entry));
        }
        if (day.day != 0 && !day.bags.empty()) days.push_back(std::move(day));
    }
    std::sort(days.begin(), days.end(), [](BackupDay const& a, BackupDay const& b) { return a.day > b.day; });
    return days;
}

ojson daysToJson(std::vector<BackupDay> const& days) {
    ojson daysJson = ojson::array();
    for (auto const& day : days) {
        ojson bagsJson = ojson::array();
        for (auto const& bag : day.bags) {
            bagsJson.push_back(ojson{{"time", bag.time}, {"note", bag.note}});
        }
        daysJson.push_back(ojson{{"day", day.day}, {"bags", std::move(bagsJson)}});
    }
    return daysJson;
}

fs::path playerIndexPath(fs::path const& root, std::string const& uuid) {
    return root / uuid / kIndexFileName;
}

// data/index.json（全局索引）拆成 per-player 目录
void migrateGlobalIndex(fs::path const& dataDir, fs::path const& root) {
    auto const old = dataDir / kIndexFileName;
    if (!fs::exists(old)) return;

    auto const json = parseJsonFile(old);
    if (!json) {
        renameAside(old, brokenName(old), "旧全局备份索引解析失败");
        return;
    }

    auto const   players = json->value("players", ojson::object());
    size_t       written = 0;
    std::error_code ec;
    for (auto it = players.begin(); it != players.end(); ++it) {
        auto const& uuid = it.key();
        auto        days = parseDays(it.value());
        if (days.empty()) continue;

        auto const dir = root / uuid;
        ensureDirectory(dir);
        ojson rootJson{{"version", 1}, {"uuid", uuid}, {"days", daysToJson(days)}};
        if (!writeFileAtomic(dir / kIndexFileName, rootJson.dump(2))) {
            getLogger().error("迁移备份索引失败，保持旧文件不动: {}", uuid);
            return; // 迁移整体放弃，下次启动再试
        }
        ++written;
    }
    renameAside(old, old.string() + ".migrated-" + std::to_string(nowMs()), "旧全局备份索引");
    getLogger().info("备份索引已按玩家拆分：{} 名玩家", written);
    fs::remove(old, ec); // 上面 rename 成功的话这句是空操作
}

void migrateStagedIndex(fs::path const& dataDir) {
    auto const old = dataDir / "staged.json";
    if (!fs::exists(old)) return;
    auto const target = dataDir / "staged" / kIndexFileName;
    if (fs::exists(target)) {
        renameAside(old, old.string() + ".stale-" + std::to_string(nowMs()), "旧暂存索引");
        return;
    }
    ensureDirectory(dataDir / "staged");
    std::error_code ec;
    fs::rename(old, target, ec);
    if (ec) {
        getLogger().warn("暂存索引迁移失败（{}）: {}", ec.message(), old.string());
    } else {
        getLogger().info("暂存索引已迁移到 {}", target.string());
    }
}

// 老的「export/ 下直接摊平」布局挪进独立批次目录，避免和新的 export/<时间戳>/ 混在一起
void migrateExportDir(fs::path const& dataDir) {
    auto const dir   = dataDir / "export";
    auto const index = dir / kIndexFileName;
    if (!fs::exists(index)) return;

    std::string stamp = "legacy";
    if (auto const json = parseJsonFile(index)) {
        auto const at = json->value("exportedAt", std::string{});
        if (!at.empty()) {
            stamp = at;
            for (auto& ch : stamp) {
                if (ch == ' ' || ch == ':') ch = (ch == ' ' ? '_' : '-');
                else if (ch == '.') { // 截掉小数秒
                    stamp = stamp.substr(0, &ch - stamp.data());
                    break;
                }
            }
        }
    }
    auto target = dir / stamp;
    for (int suffix = 2; fs::exists(target); ++suffix) target = dir / (stamp + "_" + std::to_string(suffix));

    std::error_code ec;
    fs::create_directories(target, ec);
    if (ec) {
        getLogger().warn("旧导出目录迁移跳过（{}）: {}", ec.message(), target.string());
        return;
    }

    size_t moved = 0;
    for (auto const& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file(ec)) continue;
        std::error_code moveEc;
        fs::rename(entry.path(), target / entry.path().filename(), moveEc);
        if (!moveEc) ++moved;
    }
    if (moved > 0) {
        getLogger().info("旧导出文件已归入批次目录 {}（{} 个文件）", target.string(), moved);
    }
}

void migrateViewCache(fs::path const& dataDir) {
    auto const meta = dataDir / "view_cache.json";
    if (!fs::exists(meta)) return;

    auto const json = parseJsonFile(meta);
    auto const bag  = dataDir / "view_cache.nbt.gz";
    auto const viewer = json ? json->value("uuid", std::string{}) : std::string{};
    if (!json || viewer.empty() || !fs::exists(bag)) {
        // 归属不明就不硬塞给某个管理员，留着让人工判断
        renameAside(meta, meta.string() + ".orphan-" + std::to_string(nowMs()), "旧查包缓存元数据");
        if (fs::exists(bag)) renameAside(bag, bag.string() + ".orphan-" + std::to_string(nowMs()), "旧查包缓存背包");
        return;
    }

    auto const dir = dataDir / "cache" / viewer;
    ensureDirectory(dir);
    std::error_code ec;
    fs::rename(bag, dir / kCacheBagName, ec);
    if (ec) {
        getLogger().warn("查包缓存迁移失败（{}）: {}", ec.message(), bag.string());
        return;
    }
    ojson out{
        {"time",      json->value("time", int64_t{0})},
        {"ownerUuid", json->value("uuid", std::string{})},
        {"ownerName", json->value("name", std::string{})},
    };
    if (!writeFileAtomic(dir / kMetaFileName, out.dump(2))) {
        getLogger().error("查包缓存元数据写入失败: {}", viewer);
    }
    fs::remove(meta, ec);
    getLogger().info("查包缓存已迁移到 {}", dir.string());
}

} // namespace

bool writeBagFile(fs::path const& path, CompoundTag const& bag) {
    auto binary = bag.toBinaryNbt(true); // 小端二进制 NBT，和世界库里的玩家 NBT 同格式
    if (binary.empty()) return false;
    auto compressed = ll::compress_utils::compress(
        binary,
        ll::compress_utils::CompressFormat::Gzip,
        ll::compress_utils::CompressLevel::Best
    );
    if (!compressed) return false;
    return ll::file_utils::writeFile(path, *compressed, /*isBinary=*/true);
}

std::optional<CompoundTag> readBagFile(fs::path const& path) {
    auto raw = ll::file_utils::readFile(path, /*isBinary=*/true);
    if (!raw || raw->empty()) return std::nullopt;
    auto binary = ll::compress_utils::decompress(*raw, ll::compress_utils::CompressFormat::Gzip);
    if (!binary) return std::nullopt;
    auto tag = CompoundTag::fromBinaryNbt(*binary, true);
    if (!tag) return std::nullopt;
    return std::move(*tag);
}

void ensureDataLayout(fs::path const& dataDir) {
    ensureDirectory(dataDir);
    auto const root = dataDir / "backups";
    ensureDirectory(root);

    migrateGlobalIndex(dataDir, root);
    migrateStagedIndex(dataDir);
    migrateExportDir(dataDir);
    migrateViewCache(dataDir);

    ojson store{{"dataVersion", kDataVersion}};
    if (!writeFileAtomic(dataDir / kStoreFileName, store.dump(2))) {
        getLogger().warn("写入 {} 失败（不影响使用）", kStoreFileName);
    }
}

// ------------------------------------------------------------------ BackupStore

bool BackupStore::load(fs::path const& dataDir) {
    mRoot = dataDir / "backups";
    mIndex.clear();
    ensureDataLayout(dataDir);
    ensureDirectory(mRoot);
    return true;
}

fs::path BackupStore::playerDir(std::string const& uuid) const { return mRoot / uuid; }

fs::path BackupStore::indexPath(std::string const& uuid) const {
    return playerIndexPath(mRoot, uuid);
}

fs::path BackupStore::bagPath(std::string const& uuid, int64_t time) const {
    return playerDir(uuid) / (std::to_string(time) + kBagFileSuffix);
}

std::vector<BackupDay>& BackupStore::indexOf(std::string const& uuid, bool* justLoaded) {
    if (justLoaded) *justLoaded = false;
    if (auto it = mIndex.find(uuid); it != mIndex.end()) return it->second;

    if (justLoaded) *justLoaded = true;
    auto& days = mIndex[uuid];

    auto const path = indexPath(uuid);
    auto const json = parseJsonFile(path);
    if (!json) {
        if (fs::exists(path)) renameAside(path, brokenName(path), "玩家备份索引解析失败");
        return days;
    }
    days = parseDays(json->value("days", ojson::array()));
    return days;
}

bool BackupStore::saveIndex(std::string const& uuid) {
    auto it = mIndex.find(uuid);
    if (it == mIndex.end()) return true;

    if (it->second.empty()) {
        std::error_code ec;
        fs::remove(indexPath(uuid), ec);
        fs::remove(playerDir(uuid), ec); // 目录里还有背包文件时不会被删掉
        return true;
    }

    ensureDirectory(playerDir(uuid));
    ojson root{{"version", 1}, {"uuid", uuid}, {"days", daysToJson(it->second)}};
    if (!writeFileAtomic(indexPath(uuid), root.dump(2))) {
        getLogger().error("写入玩家备份索引失败: {}", indexPath(uuid).string());
        return false;
    }
    return true;
}

int64_t BackupStore::create(std::string const& uuid, CompoundTag const& bag, std::string const& note) {
    if (uuid.empty()) return 0;

    auto& days = indexOf(uuid);

    int64_t time  = nowMs();
    auto    taken = [&](int64_t candidate) {
        for (auto const& day : days) {
            for (auto const& existing : day.bags) {
                if (existing.time == candidate) return true;
            }
        }
        return false;
    };
    while (taken(time)) ++time;

    auto const path = bagPath(uuid, time);
    if (!writeBagFile(path, bag)) {
        getLogger().error("写入备份文件失败: {}", path.string());
        return 0;
    }

    int64_t const today = todayStartMs();
    auto          it = std::find_if(days.begin(), days.end(), [&](BackupDay const& day) { return day.day == today; });
    if (it == days.end()) {
        days.insert(days.begin(), BackupDay{today, {}});
        it = days.begin();
    }
    it->bags.insert(it->bags.begin(), BackupEntry{time, note});

    if (!saveIndex(uuid)) {
        it->bags.erase(
            std::remove_if(it->bags.begin(), it->bags.end(), [&](BackupEntry const& e) { return e.time == time; }),
            it->bags.end()
        );
        std::error_code ec;
        fs::remove(path, ec);
        return 0;
    }
    return time;
}

std::optional<CompoundTag> BackupStore::read(std::string const& uuid, int64_t time) const {
    return readBagFile(bagPath(uuid, time));
}

bool BackupStore::erase(std::string const& uuid, int64_t time) {
    if (!fs::exists(indexPath(uuid))) return false;
    auto& days = indexOf(uuid);

    bool removed = false;
    for (auto dayIt = days.begin(); dayIt != days.end();) {
        auto& bags   = dayIt->bags;
        auto  before = bags.size();
        bags.erase(
            std::remove_if(bags.begin(), bags.end(), [&](BackupEntry const& e) { return e.time == time; }),
            bags.end()
        );
        if (bags.size() != before) removed = true;
        if (bags.empty()) {
            dayIt = days.erase(dayIt);
        } else {
            ++dayIt;
        }
    }
    if (!removed) return false;

    std::error_code ec;
    fs::remove(bagPath(uuid, time), ec);
    return saveIndex(uuid);
}

bool BackupStore::setNote(std::string const& uuid, int64_t time, std::string const& note) {
    if (!fs::exists(indexPath(uuid))) return false;
    for (auto& day : indexOf(uuid)) {
        for (auto& bag : day.bags) {
            if (bag.time == time) {
                bag.note = note;
                return saveIndex(uuid);
            }
        }
    }
    return false;
}

std::vector<BackupDay> const* BackupStore::daysOf(std::string const& uuid) {
    if (!fs::exists(indexPath(uuid))) return nullptr;
    auto& days = indexOf(uuid);
    if (days.empty()) return nullptr;
    return &days;
}

size_t BackupStore::countOf(std::string const& uuid) {
    if (!fs::exists(indexPath(uuid))) return 0;
    size_t total = 0;
    for (auto const& day : indexOf(uuid)) total += day.bags.size();
    return total;
}

std::vector<std::string> BackupStore::indexedUuids() const {
    std::vector<std::string> uuids;
    std::error_code          ec;
    if (!fs::is_directory(mRoot, ec)) return uuids;

    for (auto const& entry : fs::directory_iterator(mRoot, ec)) {
        if (!entry.is_directory(ec)) continue;
        if (!fs::exists(entry.path() / kIndexFileName, ec)) continue;
        uuids.push_back(entry.path().filename().string());
    }
    std::sort(uuids.begin(), uuids.end());
    return uuids;
}

int BackupStore::purge(int maxKeepDays) {
    if (maxKeepDays <= 0) return 0;
    int64_t const threshold = todayStartMs() - static_cast<int64_t>(maxKeepDays - 1) * 86400000;

    int removed = 0;
    for (auto const& uuid : indexedUuids()) {
        auto& days    = indexOf(uuid);
        bool  changed = false;
        for (auto it = days.begin(); it != days.end();) {
            if (it->day < threshold) {
                for (auto const& bag : it->bags) {
                    std::error_code ec;
                    fs::remove(bagPath(uuid, bag.time), ec);
                    ++removed;
                }
                it      = days.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }
        if (changed) saveIndex(uuid);
    }
    return removed;
}

BackupStore& backups() {
    static BackupStore instance;
    return instance;
}

// ------------------------------------------------------------------ StagedStore

bool StagedStore::load(fs::path const& dataDir) {
    mRoot      = dataDir / "staged";
    mIndexPath = mRoot / kIndexFileName;
    mEntries.clear();
    ensureDirectory(mRoot);

    auto const content = ll::file_utils::readFile(mIndexPath);
    if (!content) return true;
    try {
        auto json = ojson::parse(*content);
        for (auto const& entryJson : json.value("entries", ojson::array())) {
            StagedEntry entry;
            entry.time = entryJson.value("time", int64_t{0});
            entry.name = entryJson.value("name", std::string{});
            if (entry.time != 0) mEntries.push_back(std::move(entry));
        }
    } catch (std::exception const& e) {
        getLogger().error("暂存索引解析失败: {}", e.what());
        return false;
    }
    return true;
}

bool StagedStore::save() {
    ojson entriesJson = ojson::array();
    for (auto const& entry : mEntries) {
        entriesJson.push_back(ojson{{"time", entry.time}, {"name", entry.name}});
    }
    ojson root{{"version", 1}, {"entries", std::move(entriesJson)}};
    return writeFileAtomic(mIndexPath, root.dump(2));
}

fs::path StagedStore::bagPath(int64_t time) const { return mRoot / (std::to_string(time) + kBagFileSuffix); }

int64_t StagedStore::add(std::string const& name, CompoundTag const& bag) {
    int64_t time  = nowMs();
    auto    taken = [&](int64_t candidate) {
        for (auto const& entry : mEntries) {
            if (entry.time == candidate) return true;
        }
        return false;
    };
    while (taken(time)) ++time;

    auto const path = bagPath(time);
    if (!writeBagFile(path, bag)) return 0;

    mEntries.insert(mEntries.begin(), StagedEntry{time, name});
    if (!save()) {
        mEntries.erase(
            std::remove_if(mEntries.begin(), mEntries.end(), [&](StagedEntry const& e) { return e.time == time; }),
            mEntries.end()
        );
        std::error_code ec;
        fs::remove(path, ec);
        return 0;
    }
    return time;
}

std::optional<CompoundTag> StagedStore::read(int64_t time) const { return readBagFile(bagPath(time)); }

bool StagedStore::erase(int64_t time) {
    auto before = mEntries.size();
    mEntries.erase(
        std::remove_if(mEntries.begin(), mEntries.end(), [&](StagedEntry const& e) { return e.time == time; }),
        mEntries.end()
    );
    if (mEntries.size() == before) return false;
    std::error_code ec;
    fs::remove(bagPath(time), ec);
    return save();
}

StagedStore& staged() {
    static StagedStore instance;
    return instance;
}

// ------------------------------------------------------------------ CacheStore

bool CacheStore::load(fs::path const& dataDir) {
    mRoot = dataDir / "cache";
    ensureDirectory(mRoot);
    return true;
}

fs::path CacheStore::dirOf(std::string const& viewerUuid) const { return mRoot / viewerUuid; }

fs::path CacheStore::metaPath(std::string const& viewerUuid) const { return dirOf(viewerUuid) / kMetaFileName; }

fs::path CacheStore::bagPath(std::string const& viewerUuid) const { return dirOf(viewerUuid) / kCacheBagName; }

namespace {

struct CacheMeta {
    int64_t     time = 0;
    std::string ownerUuid;
    std::string ownerName;
};

std::optional<CacheMeta> readMeta(fs::path const& path) {
    auto const content = ll::file_utils::readFile(path);
    if (!content) return std::nullopt;
    try {
        auto           json = ojson::parse(*content);
        CacheMeta      meta;
        meta.time      = json.value("time", int64_t{0});
        meta.ownerUuid = json.value("ownerUuid", std::string{});
        meta.ownerName = json.value("ownerName", std::string{});
        return meta;
    } catch (std::exception const&) {
        return std::nullopt;
    }
}

} // namespace

bool CacheStore::has(std::string const& viewerUuid) const { return time(viewerUuid) != 0; }

int64_t CacheStore::time(std::string const& viewerUuid) const {
    auto const meta = readMeta(metaPath(viewerUuid));
    if (!meta || meta->time == 0) return 0;
    return fs::exists(bagPath(viewerUuid)) ? meta->time : 0;
}

std::string CacheStore::ownerUuid(std::string const& viewerUuid) const {
    auto const meta = readMeta(metaPath(viewerUuid));
    return meta ? meta->ownerUuid : std::string{};
}

std::string CacheStore::ownerName(std::string const& viewerUuid) const {
    auto const meta = readMeta(metaPath(viewerUuid));
    return meta ? meta->ownerName : std::string{};
}

std::optional<CompoundTag> CacheStore::bag(std::string const& viewerUuid) const { return readBagFile(bagPath(viewerUuid)); }

bool CacheStore::set(
    std::string const& viewerUuid,
    std::string const& ownerUuid,
    std::string const& ownerName,
    CompoundTag const& bag
) {
    if (viewerUuid.empty()) return false;
    ensureDirectory(dirOf(viewerUuid));
    if (!writeBagFile(bagPath(viewerUuid), bag)) return false;

    ojson root{
        {"time",      nowMs()   },
        {"ownerUuid", ownerUuid },
        {"ownerName", ownerName },
    };
    return writeFileAtomic(metaPath(viewerUuid), root.dump(2));
}

void CacheStore::clear(std::string const& viewerUuid) {
    std::error_code ec;
    fs::remove(bagPath(viewerUuid), ec);
    fs::remove(metaPath(viewerUuid), ec);
    fs::remove(dirOf(viewerUuid), ec); // 目录空才会被删掉
}

CacheStore& cacheStore() {
    static CacheStore instance;
    return instance;
}

} // namespace ib
