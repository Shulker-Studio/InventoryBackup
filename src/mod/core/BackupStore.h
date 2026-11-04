#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mc/deps/nbt/CompoundTag.h"

namespace ib {

// 持久化布局版本（和配置里的 config.version 是两套独立契约）
constexpr int kDataVersion = 2;

struct BackupEntry {
    int64_t     time = 0;
    std::string note;
};

struct BackupDay {
    int64_t                  day = 0; // 当天 00:00（毫秒）
    std::vector<BackupEntry> bags;
};

struct StagedEntry {
    int64_t     time = 0;
    std::string name;
};

// 背包文件读写：<dir>/<time>.nbt.gz（gzip 压缩的玩家背包 NBT）
bool writeBagFile(std::filesystem::path const& path, CompoundTag const& bag);

std::optional<CompoundTag> readBagFile(std::filesystem::path const& path);

// 把老布局（data/index.json、staged.json、view_cache.*）迁到新布局，并按需写 data/store.json
void ensureDataLayout(std::filesystem::path const& dataDir);

// 备份本体 + 每个玩家一个索引：
//   data/backups/<uuid>/index.json
//   data/backups/<uuid>/<time>.nbt.gz
class BackupStore {
public:
    bool load(std::filesystem::path const& dataDir);

    // 成功返回备份时间戳（毫秒），失败返回 0
    int64_t create(std::string const& uuid, CompoundTag const& bag, std::string const& note);

    std::optional<CompoundTag> read(std::string const& uuid, int64_t time) const;

    bool erase(std::string const& uuid, int64_t time);

    bool setNote(std::string const& uuid, int64_t time, std::string const& note);

    // 需要时才读该玩家的索引文件；没有备份返回 nullptr
    std::vector<BackupDay> const* daysOf(std::string const& uuid);

    size_t countOf(std::string const& uuid);

    // 直接枚举 backups/<uuid>/index.json，不依赖内存索引
    [[nodiscard]] std::vector<std::string> indexedUuids() const;

    // 清理超过 maxKeepDays 天的备份，返回删除的条数（maxKeepDays <= 0 表示不清理）
    int purge(int maxKeepDays);

private:
    [[nodiscard]] std::filesystem::path playerDir(std::string const& uuid) const;
    [[nodiscard]] std::filesystem::path indexPath(std::string const& uuid) const;
    [[nodiscard]] std::filesystem::path bagPath(std::string const& uuid, int64_t time) const;

    // 懒加载：第一次访问某个玩家时才读他的索引
    std::vector<BackupDay>& indexOf(std::string const& uuid, bool* justLoaded = nullptr);
    bool                    saveIndex(std::string const& uuid);

    std::filesystem::path mRoot;
    // uuid -> 已加载的天列表（节点式容器，取到的引用不会因后续插入失效）
    std::unordered_map<std::string, std::vector<BackupDay>> mIndex;
};

BackupStore& backups();

// 暂存背包（管理员手动收藏的背包）：
//   data/staged/index.json
//   data/staged/<time>.nbt.gz
class StagedStore {
public:
    bool load(std::filesystem::path const& dataDir);

    bool save();

    // 成功返回时间戳，失败返回 0
    int64_t add(std::string const& name, CompoundTag const& bag);

    std::optional<CompoundTag> read(int64_t time) const;

    bool erase(int64_t time);

    [[nodiscard]] std::vector<StagedEntry> const& entries() const { return mEntries; }

private:
    [[nodiscard]] std::filesystem::path bagPath(int64_t time) const;

    std::filesystem::path    mRoot;
    std::filesystem::path    mIndexPath;
    std::vector<StagedEntry> mEntries; // 新的在前
};

StagedStore& staged();

// 查包前缓存：按查看者隔离，多个管理员互不覆盖
//   data/cache/<viewerUuid>/meta.json
//   data/cache/<viewerUuid>/bag.nbt.gz
class CacheStore {
public:
    bool load(std::filesystem::path const& dataDir);

    [[nodiscard]] bool                has(std::string const& viewerUuid) const;
    [[nodiscard]] int64_t             time(std::string const& viewerUuid) const;
    [[nodiscard]] std::string         ownerUuid(std::string const& viewerUuid) const;
    [[nodiscard]] std::string         ownerName(std::string const& viewerUuid) const;
    [[nodiscard]] std::optional<CompoundTag> bag(std::string const& viewerUuid) const;

    bool set(
        std::string const& viewerUuid,
        std::string const& ownerUuid,
        std::string const& ownerName,
        CompoundTag const& bag
    );
    void clear(std::string const& viewerUuid);

private:
    [[nodiscard]] std::filesystem::path dirOf(std::string const& viewerUuid) const;
    [[nodiscard]] std::filesystem::path metaPath(std::string const& viewerUuid) const;
    [[nodiscard]] std::filesystem::path bagPath(std::string const& viewerUuid) const;

    std::filesystem::path mRoot;
};

CacheStore& cacheStore();

} // namespace ib
