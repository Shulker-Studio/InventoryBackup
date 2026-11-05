#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "mc/deps/nbt/CompoundTag.h"

class Player;

namespace ib {

// 备份一名玩家（在线取内存快照，离线读世界库）。返回备份时间戳表示已受理，写盘在后台异步完成
int64_t backupPlayer(std::string const& uuid, std::string const& note);

// 同步版：请求返回时文件和索引都已落盘。「先备份再覆盖」这种必须确认安全的场景用这个
int64_t backupPlayerSync(std::string const& uuid, std::string const& note);

// 备份所有在线玩家，返回成功条数
int backupOnlinePlayers(std::string const& note);

// 把背包回档到指定玩家（在线立即生效并同步世界库 / 离线改世界库）
bool restoreToPlayer(std::string const& uuid, CompoundTag const& bag, std::string* error = nullptr);

// 查包：先把查看者自己的背包缓存起来，再把目标背包覆盖到查看者身上
bool viewBagAs(Player& viewer, CompoundTag const& bag, std::string* error = nullptr);

// 把缓存里的自己背包覆盖回来
bool restoreViewCache(Player& viewer, std::string* error = nullptr);

struct PortResult {
    int                success = 0;
    int                failed  = 0;
    std::string        dir; // 导出/导入实际使用的批次目录
};

// 导出 / 导入：每一批一个目录
//   data/export/<2026-11-04_15-30-00>/index.json
//   data/export/<2026-11-04_15-30-00>/<uuid>.nbt.gz
std::filesystem::path exportDir();

// 当前有效批次目录（最新的一批）；没有导出过返回空
std::filesystem::path currentExportBatch();

std::filesystem::path exportBatchDir(std::string const& batch);

std::vector<std::string> listExportBatches();

bool exportFileExists();

// 导出 / 导入都很慢（要挨个读世界库、压缩、落盘），所以做成「立刻回一条开始提示 + 分片执行 + 进度 + 完成提示」：
// notifyUuid 是收提示的管理员（空字符串表示只写日志），onDone 在完成时回调（参数带结果与批次目录）
using PortDone = std::function<void(PortResult const&)>;

void startExportAll(std::string const& notifyUuid, PortDone onDone = {});

void startImportAll(std::string const& notifyUuid, PortDone onDone = {});

// 是否有导出/导入任务正在跑
bool portBusy();

// 外部背包清单（只读索引，背包正文按需读）
struct ExternalBagEntry {
    std::string uuid;
    std::string xuid;
    std::string name;
};

bool externalBagsAvailable();

std::vector<ExternalBagEntry> listExternalBags();

std::optional<CompoundTag> readExternalBag(std::string const& uuid);

} // namespace ib
