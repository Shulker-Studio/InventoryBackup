#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "mc/deps/nbt/CompoundTag.h"
#include "mod/config/Config.h"

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

// 背包文件的编解码原语，按线程亲和性分开：
//   encode/decode 碰 NBT 对象，只在服务端线程用；
//   deflate/inflate/readBinary/writeBinary 是纯 CPU 或纯 I/O，可以放到线程池。
// 后台任务只能拿到字节串，不能拿 CompoundTag 去跨线程传。
namespace bagio {
// 档位定义在 Config.h（配置层是唯一来源），这边只是别名
using Level = CompressLevel;

Level                      levelFrom(std::string const& name);                                    // 服务端线程：配置字符串 -> 档位
std::string                encode(CompoundTag const& bag);                                        // 服务端线程：NBT -> 小端二进制
std::optional<CompoundTag> decode(std::string_view binary);                                       // 服务端线程：小端二进制 -> NBT
std::string                deflate(std::string_view binary, Level level);                         // 可后台：gzip 压缩，失败返回空
std::optional<std::string> inflate(std::string_view raw);                                         // 可后台：gzip 解压
bool                       readBinary(std::filesystem::path const& path, std::string& out);       // 可后台：读文件
bool                       writeBinary(std::filesystem::path const& path, std::string_view data); // 可后台：写文件
} // namespace bagio

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
    // 建一份备份：NBT -> 字节串在调用线程做，压缩与写盘丢后台，索引等文件落盘后回主线程提交。
    // 返回时间戳表示「已受理」，不是「已写盘」；需要确保已落盘的场景用 createSync
    int64_t create(std::string const& uuid, CompoundTag const& bag, std::string const& note);

    // 同步版：写完文件和索引才返回。只有「先备份再覆盖」这种必须确认安全的场景才用
    int64_t createSync(std::string const& uuid, CompoundTag const& bag, std::string const& note);

    std::optional<CompoundTag> read(std::string const& uuid, int64_t time) const;

    bool erase(std::string const& uuid, int64_t time);

    bool setNote(std::string const& uuid, int64_t time, std::string const& note);

    // 需要时才读该玩家的索引文件；没有备份返回 nullptr
    std::vector<BackupDay> const* daysOf(std::string const& uuid);

    size_t countOf(std::string const& uuid);

    // 直接枚举 backups/<uuid>/index.json，不依赖内存索引
    [[nodiscard]] std::vector<std::string> indexedUuids() const;

    // 过期清理，按玩家切片：uuids 是待处理的玩家列表，cursor 是进度（进出双向）；
    // 本次最多花 budget 时间，返回本次删掉的备份数。maxKeepDays <= 0 时直接跳过（cursor 推到末尾）
    size_t purgeBatch(
        std::vector<std::string> const& uuids,
        size_t&                         cursor,
        int                             maxKeepDays,
        std::chrono::milliseconds       budget
    );

    // 索引缓存上限：超过 limit 就清到一半（limit = 0 表示不限）。
    // 只能在没人持有 indexOf 返回的引用时调（比如调度器每拍末尾）
    void trimIndexCache(size_t limit);

private:
    [[nodiscard]] std::filesystem::path playerDir(std::string const& uuid) const;
    [[nodiscard]] std::filesystem::path indexPath(std::string const& uuid) const;
    [[nodiscard]] std::filesystem::path bagPath(std::string const& uuid, int64_t time) const;

    // 懒加载：第一次访问某个玩家时才读他的索引
    std::vector<BackupDay>& indexOf(std::string const& uuid, bool* justLoaded = nullptr);
    bool                    saveIndex(std::string const& uuid);

    // 分配一个没被占用的时间戳（同一毫秒内多次备份会顺延，保证背包文件名不撞）
    static int64_t allocateTime(std::vector<BackupDay> const& days);
    // 把这条备份记进内存（挂在当天这一桶的最前面）
    static void reserve(std::vector<BackupDay>& days, int64_t time, std::string const& note);
    // 只撤内存里的占位，返回是否真的删掉了
    bool removeEntry(std::string const& uuid, int64_t time);

    // 后台写盘回来的收尾（服务端线程）：成功才提交索引，失败就把占位和文件撤掉
    void finishCreate(std::string const& uuid, int64_t time, bool ok);
    // 撤占位 + 删文件 + 必要时重写索引（别的备份可能已经把这条一起写进索引了）
    void rollbackCreate(std::string const& uuid, int64_t time);

    // 清理单个玩家的过期天，返回删掉的备份数；清完就把他的索引从缓存里踢掉
    size_t purgeOne(std::string const& uuid, int64_t threshold);

    std::filesystem::path mRoot;
    // uuid -> 已加载的天列表（节点式容器，取到的引用不会因后续插入失效）
    std::unordered_map<std::string, std::vector<BackupDay>> mIndex;
    // uuid -> 还在后台写盘的备份时间戳。它们既是「不许踢出 mIndex」的理由
    // （踢了回来提交索引会失败），也是「先别写进索引」的理由（文件还没落盘）
    std::unordered_map<std::string, std::vector<int64_t>> mPendingWrites;
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
