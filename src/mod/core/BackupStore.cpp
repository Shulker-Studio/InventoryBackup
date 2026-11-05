#include "mod/core/BackupStore.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <system_error>

#include "ll/api/io/FileUtils.h"
#include "ll/api/io/Logger.h"
#include "ll/api/utils/CompressUtils.h"
#include "mod/Mod.h"
#include "mod/core/AsyncIo.h"
#include "mod/core/Util.h"
#include "mod/i18n/Text.h"

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

void renameAside(fs::path const& path, std::string const& target, char const* whatKey) {
    std::error_code ec;
    fs::rename(path, target, ec);
    if (ec) {
        getLogger().warn(msg("log.store.rename_failed", msg(whatKey), ec.message(), formatPath(path)));
    } else {
        getLogger().warn(msg("log.store.renamed", msg(whatKey), formatPath(target)));
    }
}

// 读全局 players 索引（老布局 / 迁移用）
std::optional<ojson> parseJsonFile(fs::path const& path) {
    auto const content = ll::file_utils::readFile(path);
    if (!content) return std::nullopt;
    try {
        return ojson::parse(*content);
    } catch (std::exception const& e) {
        getLogger().error(msg("log.store.json_parse_failed", formatPath(path), e.what()));
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

// 文件还没落盘的那几条先不写进索引：崩在这中间只会留个没人引用的孤儿文件，
// 不会出现「索引里有、文件打不开」的备份
ojson daysToJson(std::vector<BackupDay> const& days, std::vector<int64_t> const& pending = {}) {
    auto const isPending = [&](int64_t time) {
        return std::find(pending.begin(), pending.end(), time) != pending.end();
    };
    ojson daysJson = ojson::array();
    for (auto const& day : days) {
        ojson bagsJson = ojson::array();
        for (auto const& bag : day.bags) {
            if (isPending(bag.time)) continue;
            bagsJson.push_back(ojson{{"time", bag.time}, {"note", bag.note}});
        }
        if (bagsJson.empty()) continue;
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
        renameAside(old, brokenName(old), "log.store.what.corrupt_legacy_index");
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
        if (!writeFileAtomic(dir / kIndexFileName, rootJson.dump())) {
            getLogger().error(msg("log.store.index_migrate_failed", uuid));
            return; // 迁移整体放弃，下次启动再试
        }
        ++written;
    }
    renameAside(old, old.string() + ".migrated-" + std::to_string(nowMs()), "log.store.what.legacy_index");
    getLogger().info(msg("log.store.index_split", written));
    fs::remove(old, ec); // 上面 rename 成功的话这句是空操作
}

void migrateStagedIndex(fs::path const& dataDir) {
    auto const old = dataDir / "staged.json";
    if (!fs::exists(old)) return;
    auto const target = dataDir / "staged" / kIndexFileName;
    if (fs::exists(target)) {
        renameAside(old, old.string() + ".stale-" + std::to_string(nowMs()), "log.store.what.staged_index");
        return;
    }
    ensureDirectory(dataDir / "staged");
    std::error_code ec;
    fs::rename(old, target, ec);
    if (ec) {
        getLogger().warn(msg("log.store.staged_migrate_failed", ec.message(), formatPath(old)));
    } else {
        getLogger().info(msg("log.store.staged_migrated", formatPath(target)));
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
        getLogger().warn(msg("log.store.legacy_export_skip", ec.message(), formatPath(target)));
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
        getLogger().info(msg("log.store.legacy_export_moved", formatPath(target), moved));
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
        renameAside(meta, meta.string() + ".orphan-" + std::to_string(nowMs()), "log.store.what.view_cache_meta");
        if (fs::exists(bag)) renameAside(bag, bag.string() + ".orphan-" + std::to_string(nowMs()), "log.store.what.view_cache_bag");
        return;
    }

    auto const dir = dataDir / "cache" / viewer;
    ensureDirectory(dir);
    std::error_code ec;
    fs::rename(bag, dir / kCacheBagName, ec);
    if (ec) {
        getLogger().warn(msg("log.store.view_cache_migrate_failed", ec.message(), formatPath(bag)));
        return;
    }
    ojson out{
        {"time",      json->value("time", int64_t{0})},
        {"ownerUuid", json->value("uuid", std::string{})},
        {"ownerName", json->value("name", std::string{})},
    };
    if (!writeFileAtomic(dir / kMetaFileName, out.dump())) {
        getLogger().error(msg("log.store.view_cache_meta_write_failed", viewer));
    }
    fs::remove(meta, ec);
    getLogger().info(msg("log.store.view_cache_migrated", formatPath(dir)));
}

} // namespace

namespace bagio {

std::string encode(CompoundTag const& bag) { return bag.toBinaryNbt(true); }

std::optional<CompoundTag> decode(std::string_view binary) {
    auto tag = CompoundTag::fromBinaryNbt(binary, true);
    if (!tag) return std::nullopt;
    return std::move(*tag);
}

// 配置里的三个档位对应 zlib 的 1 / 6 / 9；best 体积只比 default 小 0.4%，但慢近 5 倍。
// 档位作为参数传进来，后台线程就不用去读 Config（那是服务端线程在改的）
Level levelFrom(std::string const& name) { return compressLevelFromName(name).value_or(Level::Default); }

std::string deflate(std::string_view binary, Level level) {
    using ll::compress_utils::CompressLevel;
    auto const mapped = level == Level::Fastest ? CompressLevel::Fastest
                      : level == Level::Best    ? CompressLevel::Best
                                                : CompressLevel::Default;
    auto compressed = ll::compress_utils::compress(binary, ll::compress_utils::CompressFormat::Gzip, mapped);
    if (!compressed) return {};
    return std::move(*compressed);
}

std::optional<std::string> inflate(std::string_view raw) {
    auto binary = ll::compress_utils::decompress(raw, ll::compress_utils::CompressFormat::Gzip);
    if (!binary) return std::nullopt;
    return std::move(*binary);
}

bool readBinary(fs::path const& path, std::string& out) {
    auto raw = ll::file_utils::readFile(path, /*isBinary=*/true);
    if (!raw || raw->empty()) return false;
    out = std::move(*raw);
    return true;
}

bool writeBinary(fs::path const& path, std::string_view data) {
    return ll::file_utils::writeFile(path, data, /*isBinary=*/true);
}

} // namespace bagio

bool writeBagFile(fs::path const& path, CompoundTag const& bag) {
    auto const binary = bagio::encode(bag);
    if (binary.empty()) return false;
    auto const compressed = bagio::deflate(binary, bagio::levelFrom(currentConfig().compressLevel));
    if (compressed.empty()) return false;
    return bagio::writeBinary(path, compressed);
}

std::optional<CompoundTag> readBagFile(fs::path const& path) {
    std::string raw;
    if (!bagio::readBinary(path, raw)) return std::nullopt;
    auto binary = bagio::inflate(raw);
    if (!binary) return std::nullopt;
    return bagio::decode(*binary);
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
    if (!writeFileAtomic(dataDir / kStoreFileName, store.dump())) {
        getLogger().warn(msg("log.store.write_failed", kStoreFileName));
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
        if (fs::exists(path)) renameAside(path, brokenName(path), "log.store.what.corrupt_player_index");
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
    std::vector<int64_t> const none;
    auto const                pendingIt = mPendingWrites.find(uuid);
    auto const& pending = pendingIt == mPendingWrites.end() ? none : pendingIt->second;
    ojson root{{"version", 1}, {"uuid", uuid}, {"days", daysToJson(it->second, pending)}};
    if (!writeFileAtomic(indexPath(uuid), root.dump())) {
        getLogger().error(msg("log.store.player_index_write_failed", formatPath(indexPath(uuid))));
        return false;
    }
    return true;
}

int64_t BackupStore::create(std::string const& uuid, CompoundTag const& bag, std::string const& note) {
    if (uuid.empty()) return 0;

    auto&   days = indexOf(uuid);
    int64_t time = allocateTime(days);

    // NBT -> 字节串只在服务端线程做；之后压缩、写盘都丢后台
    auto const binary = bagio::encode(bag);
    if (binary.empty()) return 0;

    auto const path = bagPath(uuid, time);

    // 先在内存里占位：时间戳去重能看见它，表单也立刻就有这条备份
    reserve(days, time, note);

    // 文件先落盘、索引后提交：和以前一样，崩在中间最多留个没人引用的孤儿文件，
    // 不会出现「索引里有、文件打不开」的备份
    ensureDirectory(playerDir(uuid)); // 后台写盘不会建目录，得在这边先建好
    mPendingWrites[uuid].push_back(time);
    auto const level = bagio::levelFrom(currentConfig().compressLevel);
    auto*      self  = this;
    runIo([self, uuid, time, path, binary, level]() mutable {
        auto const compressed = bagio::deflate(binary, level);
        bool const ok         = !compressed.empty() && bagio::writeBinary(path, compressed);
        backToServer([self, ok, uuid, time]() { self->finishCreate(uuid, time, ok); });
    });
    return time;
}

// 同步版：老实现原封不动留在这里，给「先备份再覆盖」那种必须确认落盘的场景用
int64_t BackupStore::createSync(std::string const& uuid, CompoundTag const& bag, std::string const& note) {
    if (uuid.empty()) return 0;

    auto&   days = indexOf(uuid);
    int64_t time = allocateTime(days);

    auto const path = bagPath(uuid, time);
    ensureDirectory(playerDir(uuid));
    if (!writeBagFile(path, bag)) {
        getLogger().error(msg("log.store.backup_write_failed", formatPath(path)));
        return 0;
    }

    reserve(days, time, note);

    if (!saveIndex(uuid)) {
        // 索引没写进去，磁盘上还是干净的，所以只撤内存和文件，不用再写一次索引
        removeEntry(uuid, time);
        std::error_code ec;
        fs::remove(path, ec);
        return 0;
    }
    return time;
}

void BackupStore::finishCreate(std::string const& uuid, int64_t time, bool ok) {
    if (auto pending = mPendingWrites.find(uuid); pending != mPendingWrites.end()) {
        auto& times = pending->second;
        times.erase(std::remove(times.begin(), times.end(), time), times.end());
        if (times.empty()) mPendingWrites.erase(pending);
    }
    auto const path = bagPath(uuid, time);
    if (!ok) {
        getLogger().error(msg("log.store.backup_write_failed", formatPath(path)));
        rollbackCreate(uuid, time);
        return;
    }
    // 这期间用户可能已经把这条删掉了：内存里没有它就删掉刚写的文件，别留孤儿
    bool const stillThere = [&] {
        auto it = mIndex.find(uuid);
        if (it == mIndex.end()) return false;
        for (auto const& day : it->second) {
            for (auto const& entry : day.bags) {
                if (entry.time == time) return true;
            }
        }
        return false;
    }();
    if (!stillThere) {
        std::error_code ec;
        fs::remove(path, ec);
        return;
    }

    // 索引提交留在服务端线程；写不进去就把占位和文件一起撤掉，别留下看得见打不开的备份
    if (!saveIndex(uuid)) rollbackCreate(uuid, time);
}

int64_t BackupStore::allocateTime(std::vector<BackupDay> const& days) {
    auto taken = [&](int64_t candidate) {
        for (auto const& day : days) {
            for (auto const& existing : day.bags) {
                if (existing.time == candidate) return true;
            }
        }
        return false;
    };
    int64_t time = nowMs();
    while (taken(time)) ++time;
    return time;
}

void BackupStore::reserve(std::vector<BackupDay>& days, int64_t time, std::string const& note) {
    int64_t const today = todayStartMs();
    auto          it = std::find_if(days.begin(), days.end(), [&](BackupDay const& day) { return day.day == today; });
    if (it == days.end()) {
        days.insert(days.begin(), BackupDay{today, {}});
        it = days.begin();
    }
    it->bags.insert(it->bags.begin(), BackupEntry{time, note});
}

bool BackupStore::removeEntry(std::string const& uuid, int64_t time) {
    auto it = mIndex.find(uuid);
    if (it == mIndex.end()) return false;
    auto& days = it->second;
    for (auto day = days.begin(); day != days.end(); ++day) {
        auto& bags = day->bags;
        auto  pos  = std::find_if(bags.begin(), bags.end(), [&](BackupEntry const& e) { return e.time == time; });
        if (pos == bags.end()) continue;
        bags.erase(pos);
        if (bags.empty()) days.erase(day);
        return true;
    }
    return false;
}

void BackupStore::rollbackCreate(std::string const& uuid, int64_t time) {
    bool const removed = removeEntry(uuid, time);
    std::error_code ec;
    fs::remove(bagPath(uuid, time), ec);
    // 这期间同一玩家别的备份可能已经把这条一起写进索引了，得再写一次把它撤回来
    if (removed) saveIndex(uuid);
}

std::optional<CompoundTag> BackupStore::read(std::string const& uuid, int64_t time) const {
    return readBagFile(bagPath(uuid, time));
}

bool BackupStore::erase(std::string const& uuid, int64_t time) {
    // 不能只看 index.json 在不在：新建玩家的第一条备份可能还在后台写盘，
    // 索引文件还没落盘，但内存里已经有这条了
    if (!mIndex.contains(uuid) && !fs::exists(indexPath(uuid))) return false;
    auto& days = indexOf(uuid);

    auto const      path = bagPath(uuid, time);
    std::error_code ec;
    fs::remove(path, ec);
    if (ec) {
        // 文件删不掉就不动索引：条目留着 UI 里还在，下次清理或再删一次还能重试。
        // 反过来直接删条目，只会把它变成谁也引用不到的孤儿文件
        getLogger().warn(msg("log.store.remove_failed", formatPath(path), ec.message()));
        if (days.empty()) mIndex.erase(uuid); // 顺手清掉为这次删除而加载的空壳
        return false;
    }
    if (!removeEntry(uuid, time)) {
        if (days.empty()) mIndex.erase(uuid);
        return false;
    }
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

size_t BackupStore::purgeBatch(
    std::vector<std::string> const& uuids,
    size_t&                         cursor,
    int                             maxKeepDays,
    std::chrono::milliseconds       budget
) {
    if (maxKeepDays <= 0) {
        cursor = uuids.size();
        return 0;
    }

    int64_t const threshold = todayStartMs() - static_cast<int64_t>(maxKeepDays - 1) * 86400000;
    auto const    begin     = std::chrono::steady_clock::now();

    size_t removed = 0;
    while (cursor < uuids.size()) {
        removed += purgeOne(uuids[cursor], threshold);
        ++cursor;
        // 预算用完就收手，剩下的等下一拍，别占着服务端线程不动
        if (std::chrono::steady_clock::now() - begin >= budget) break;
    }
    return removed;
}

void BackupStore::trimIndexCache(size_t limit) {
    if (limit == 0 || mIndex.size() <= limit) return;
    // 一次清到一半：既不会每拍都擦，也不会让内存满在刚好 limit 上反复进出。
    // 被踢掉的玩家下次访问时重新读一遍 index.json，不影响正确性
    size_t const keep = limit / 2;
    for (auto it = mIndex.begin(); it != mIndex.end() && mIndex.size() > keep;) {
        if (mPendingWrites.contains(it->first)) { // 后台写还没落地，踢了它就提交不了索引
            ++it;
            continue;
        }
        it = mIndex.erase(it);
    }
}

size_t BackupStore::purgeOne(std::string const& uuid, int64_t threshold) {
    auto& days = indexOf(uuid);

    size_t removed = 0;
    bool   dirty   = false;
    for (auto it = days.begin(); it != days.end();) {
        if (it->day >= threshold) {
            ++it;
            continue;
        }

        auto& bags = it->bags;
        bags.erase(
            std::remove_if(
                bags.begin(),
                bags.end(),
                [&](BackupEntry const& bag) {
                    std::error_code ec;
                    // 删不掉的就留在索引里，下次清理再试，兼得「索引不失忆」和「不产生孤儿文件」
                    bool const deleted = fs::remove(bagPath(uuid, bag.time), ec);
                    if (ec) {
                        getLogger().warn(
                            msg("log.store.purge_failed", formatPath(bagPath(uuid, bag.time)), ec.message())
                        );
                        return false;
                    }
                    // 文件本来就不在（上一次删除成功但索引没写进去）也算处理完了
                    if (deleted) ++removed;
                    return true;
                }
            ),
            bags.end()
        );
        dirty = true;
        if (bags.empty()) {
            it = days.erase(it);
        } else {
            ++it;
        }
    }
    if (dirty) saveIndex(uuid);

    // 清理是「过一遍就走」：全服索引挨个载进来会把它们全部变成常驻内存。
    // 有后台写还没落地时不能踢，否则它回来提交索引时会找不到自己
    if (!mPendingWrites.contains(uuid)) mIndex.erase(uuid);
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
        getLogger().error(msg("log.store.staged_parse_failed", e.what()));
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
    return writeFileAtomic(mIndexPath, root.dump());
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
    return writeFileAtomic(metaPath(viewerUuid), root.dump());
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
