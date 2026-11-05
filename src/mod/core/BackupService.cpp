#include "mod/core/BackupService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <vector>

#include "mod/core/AsyncIo.h"
#include "mod/core/BackupStore.h"
#include "mod/core/BagOps.h"
#include "mod/core/PlayerData.h"
#include "mod/core/Util.h"
#include "mod/i18n/Text.h"
#include "ll/api/io/FileUtils.h"
#include "ll/api/io/Logger.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/network/packet/TextPacket.h"
#include "mc/world/actor/player/Player.h"
#include "mod/Mod.h"

namespace ib {

namespace {

using ojson = nlohmann::ordered_json;

constexpr char kBagFileSuffix[] = ".nbt.gz";
constexpr char kIndexFileName[] = "index.json";

} // namespace

fs::path exportDir() { return Mod::getInstance().dataDir() / "export"; }

fs::path exportBatchDir(std::string const& batch) { return exportDir() / batch; }

std::vector<std::string> listExportBatches() {
    // 批次目录名就是时间戳，按名字排序即按时间排序
    std::vector<std::string> batches;
    std::error_code          ec;
    if (!fs::is_directory(exportDir(), ec)) return batches;
    for (auto const& entry : fs::directory_iterator(exportDir(), ec)) {
        if (!entry.is_directory(ec)) continue;
        if (!fs::exists(entry.path() / kIndexFileName, ec)) continue;
        batches.push_back(entry.path().filename().string());
    }
    std::sort(batches.begin(), batches.end());
    return batches;
}

fs::path currentExportBatch() {
    auto const batches = listExportBatches();
    if (!batches.empty()) return exportBatchDir(batches.back());
    // 兼容老布局：index.json 直接躺在 export/ 下
    if (fs::exists(exportDir() / kIndexFileName)) return exportDir();
    return {};
}

namespace {

fs::path indexPath() {
    auto const batch = currentExportBatch();
    return batch.empty() ? fs::path{} : batch / kIndexFileName;
}

fs::path bagPathOf(std::string const& uuid) {
    auto const batch = currentExportBatch();
    return batch.empty() ? fs::path{} : batch / (uuid + kBagFileSuffix);
}

std::string batchStamp() {
    auto stamp = formatTime(nowMs()); // 2026-11-04 15:30:00
    std::replace(stamp.begin(), stamp.end(), ' ', '_');
    std::replace(stamp.begin(), stamp.end(), ':', '-');
    return stamp;
}

} // namespace

bool exportFileExists() { return !indexPath().empty() && fs::exists(indexPath()); }

namespace {

std::optional<ojson> readExportIndex() {
    auto const path = indexPath();
    if (path.empty()) {
        getLogger().error(msg("log.service.no_export_batch", formatPath(exportDir())));
        return std::nullopt;
    }
    auto const content = ll::file_utils::readFile(path);
    if (!content) {
        getLogger().error(msg("log.service.export_index_missing", formatPath(path)));
        return std::nullopt;
    }
    try {
        return ojson::parse(*content);
    } catch (std::exception const& e) {
        getLogger().error(msg("log.service.export_index_parse_failed", e.what()));
        return std::nullopt;
    }
}

} // namespace

int64_t backupPlayer(std::string const& uuid, std::string const& note) {
    auto bag = snapshotBag(uuid);
    if (!bag) {
        getLogger().warn(msg("log.service.backup_no_bag", uuid));
        return 0;
    }
    auto time = backups().create(uuid, *bag, note);
    if (time == 0) {
        getLogger().error(msg("log.service.backup_write_failed", uuid));
    }
    return time;
}

int64_t backupPlayerSync(std::string const& uuid, std::string const& note) {
    auto bag = snapshotBag(uuid);
    if (!bag) {
        getLogger().warn(msg("log.service.backup_no_bag", uuid));
        return 0;
    }
    auto time = backups().createSync(uuid, *bag, note);
    if (time == 0) {
        getLogger().error(msg("log.service.backup_write_failed", uuid));
    }
    return time;
}

int backupOnlinePlayers(std::string const& note) {
    int  success          = 0;
    auto includeSimulated = !currentConfig().skipSimulatedPlayers;
    for (auto* player : onlinePlayers(includeSimulated)) {
        if (backupPlayer(player->getUuid().asString(), note) != 0) ++success;
    }
    return success;
}

bool restoreToPlayer(std::string const& uuid, CompoundTag const& bag, std::string* error) {
    return restoreBag(uuid, bag, error);
}

bool viewBagAs(Player& viewer, CompoundTag const& bag, std::string* error) {
    auto uuid = viewer.getUuid().asString();
    // 缓存按查看者隔离：多个管理员同时查包不会互相覆盖恢复点
    if (auto own = snapshotBag(uuid); own) {
        cacheStore().set(uuid, uuid, viewer.getRealName(), *own);
    } else {
        getLogger().warn(msg("log.service.view_cache_failed", uuid));
    }
    return restoreBag(uuid, bag, error);
}

bool restoreViewCache(Player& viewer, std::string* error) {
    auto const uuid   = viewer.getUuid().asString();
    auto       cached = cacheStore().bag(uuid);
    if (!cached) {
        if (error) *error = msg("backup.menu.restore_cache.none");
        return false;
    }
    if (!restoreBag(uuid, *cached, error)) return false;
    cacheStore().clear(uuid);
    return true;
}

namespace {

// 分片大小：每批处理这么多玩家，然后让出一次服务器线程。
// 执行器的 30ms 预算只在回调之间切，一个回调里活多干得久，所以可配（config.portChunkSize），默认很小。
// 它同时也是「同时最多几个后台任务」的上限
size_t portChunk() {
    auto const size = currentConfig().portChunkSize;
    return size < 1 ? 1 : static_cast<size_t>(size);
}

// 通知管理员：uuid 空表示只写日志
void notifyPlayer(std::string const& uuid, std::string const& text) {
    if (uuid.empty()) return;
    if (auto* player = onlinePlayer(uuid); player != nullptr) {
        auto packet = TextPacket::createRawMessage(msgPrefix() + text);
        player->sendNetworkPacket(packet);
    }
}

enum class PortKind { Export, Import };

struct PortJob {
    PortKind                  kind = PortKind::Export;
    std::string               notifyUuid;
    PortDone                  onDone;
    std::vector<PlayerRecord> records;    // 导出用
    std::vector<std::string>  targets;    // 导入用
    ojson                     index       = ojson::object();
    ojson                     playersJson = ojson::object();
    fs::path                  batch;
    PortResult                result;
    size_t                    pos      = 0;
    size_t                    pending  = 0; // 还在后台跑的个数，上限 portChunkSize
    int                       lastPct  = 0;
};

std::shared_ptr<PortJob> gJob;

void schedulePump(std::shared_ptr<PortJob> job);

void finishJob(std::shared_ptr<PortJob> const& job, std::string const& message) {
    notifyPlayer(job->notifyUuid, message);
    getLogger().info(message);
    auto const done = job->onDone;
    gJob.reset();
    if (done) done(job->result);
}

int percentOf(size_t pos, size_t total) {
    if (total == 0) return 100;
    return static_cast<int>(pos * 100 / total);
}

void reportProgress(std::shared_ptr<PortJob> const& job, size_t total) {
    int const percent = percentOf(job->pos, total);
    if (job->pos != total && percent < job->lastPct + 10) return; // 每 10% 报一次
    job->lastPct = percent;
    auto const key = job->kind == PortKind::Export ? "backup.port.export_progress" : "backup.port.import_progress";
    notifyPlayer(job->notifyUuid, msg(key, percent, job->pos, total));
}

// 收尾：所有人发完了才写索引
void advanceExport(std::shared_ptr<PortJob> const& job) {
    ojson      root{
        {"version",    1                  },
        {"exportedAt", formatTime(nowMs())},
        {"players",    std::move(job->playersJson)}
    };
    auto const indexText = root.dump(2);
    // 导出包是要给人翻的，这里保留缩进；写盘挪到后台
    runIo([job, indexText]() {
        bool const ok = ll::file_utils::writeFile(job->batch / kIndexFileName, indexText);
        backToServer([job, ok]() {
            if (gJob != job) return; // 任务已被取消/替换，安静收摊
            if (!ok) {
                getLogger().error(msg("log.service.export_index_write_failed", formatPath(job->batch / kIndexFileName)));
                job->result.failed += job->result.success;
                job->result.success = 0;
            }
            finishJob(job, msg("backup.stage.export_done", job->result.success, job->result.failed, job->result.dir));
        });
    });
}

// 泵：始终让最多 portChunkSize 个任务在后台跑，完成一个立刻补一个。
// 每批都要等一个 tick，所以"补满"比"等整批回来再派下一批"省掉一半的等待
void pumpExport(std::shared_ptr<PortJob> const& job) {
    if (gJob != job) return;

    auto const total = job->records.size();
    // 压缩档位在主线程算好再带进后台，后台不去碰 Config
    auto const level = bagio::levelFrom(currentConfig().compressLevel);

    // 服务端线程只做不能挪的那一半：从活体玩家读 NBT 并序列化成二进制字节串
    while (job->pending < portChunk() && job->pos < total) {
        auto const& record = job->records[job->pos++];
        auto        bag    = snapshotBag(record.uuid);
        if (!bag) {
            getLogger().warn(msg("log.service.export_no_bag", record.uuid));
            ++job->result.failed;
            continue;
        }

        ++job->pending;
        // 压缩 + 写盘全在后台，只把成败和不可变字段带回来
        runIo([job, uuid = record.uuid, xuid = record.xuid, name = record.name,
               path = job->batch / (record.uuid + kBagFileSuffix), binary = bagio::encode(*bag), level]() {
            auto const compressed = bagio::deflate(binary, level);
            bool const ok         = !compressed.empty() && bagio::writeBinary(path, compressed);
            backToServer([job, ok, uuid, xuid, name]() {
                if (gJob != job) return;
                if (ok) {
                    job->playersJson[uuid] = ojson{
                        {"xuid", xuid                 },
                        {"name", name                 },
                        {"file", uuid + kBagFileSuffix},
                    };
                    ++job->result.success;
                } else {
                    getLogger().warn(msg("log.service.export_bag_write_failed", uuid));
                    ++job->result.failed;
                }
                --job->pending;
                reportProgress(job, job->records.size());
                pumpExport(job); // 补下一个
            });
        });
    }

    if (job->pos >= total && job->pending == 0) advanceExport(job);
}

void advanceImport(std::shared_ptr<PortJob> const& job) {
    savePlayerCache();
    finishJob(job, msg("backup.external.import_all_done", job->result.success, job->result.failed));
}

// 泵：同 pumpExport
void pumpImport(std::shared_ptr<PortJob> const& job) {
    if (gJob != job) return;

    auto const total = job->targets.size();
    while (job->pending < portChunk() && job->pos < total) {
        auto const uuid = job->targets[job->pos++];

        ++job->pending;
        // 读盘 + 解压放后台；解出来的字节再回主线程还原成 NBT 并写回玩家数据
        runIo([job, uuid, path = job->batch / (uuid + kBagFileSuffix)] {
            std::string                raw;
            std::optional<std::string> binary;
            if (bagio::readBinary(path, raw)) binary = bagio::inflate(raw);
            backToServer([job, uuid, binary = std::move(binary)]() {
                if (gJob != job) return;
                auto bag = binary ? bagio::decode(*binary) : std::nullopt;
                if (!bag) {
                    getLogger().warn(msg("log.service.import_skipped", uuid));
                    ++job->result.failed;
                } else {
                    auto const& value = job->index[uuid];
                    rememberPlayer(uuid, value.value("xuid", std::string{}), value.value("name", std::string{}));
                    std::string error;
                    if (restoreToPlayer(uuid, *bag, &error)) {
                        ++job->result.success;
                    } else {
                        getLogger().warn(msg("log.service.import_failed", uuid, error));
                        ++job->result.failed;
                    }
                }
                --job->pending;
                reportProgress(job, job->targets.size());
                pumpImport(job); // 补下一个
            });
        });
    }

    if (job->pos >= total && job->pending == 0) advanceImport(job);
}

void pumpJob(std::shared_ptr<PortJob> job) {
    if (job->kind == PortKind::Export) {
        pumpExport(job);
    } else {
        pumpImport(job);
    }
}

// 只负责把第一批任务挂到服务端线程上，之后由完成回调自己续
void schedulePump(std::shared_ptr<PortJob> job) {
    ll::thread::ServerThreadExecutor::getDefault().execute([job]() { pumpJob(job); });
}

} // namespace

bool portBusy() { return gJob != nullptr; }

void startExportAll(std::string const& notifyUuid, PortDone onDone) {
    if (gJob) {
        notifyPlayer(notifyUuid, msg("backup.port.busy"));
        return;
    }

    auto job        = std::make_shared<PortJob>();
    job->kind       = PortKind::Export;
    job->notifyUuid = notifyUuid;
    job->onDone     = std::move(onDone);

    std::string batchName = batchStamp();
    auto        batch     = exportBatchDir(batchName);
    for (int suffix = 2; fs::exists(batch); ++suffix) {
        batch = exportBatchDir(batchName + "_" + std::to_string(suffix));
    }
    std::error_code ec;
    fs::create_directories(batch, ec);
    if (ec) {
        getLogger().error(msg("log.service.export_dir_create_failed", formatPath(batch), ec.message()));
        notifyPlayer(notifyUuid, msg("backup.error.export_dir", ec.message()));
        return;
    }
    job->batch      = batch;
    job->result.dir = formatPath(batch);
    gJob            = job;

    // 先立刻回一条「开始」，玩家列表等下一个 tick 再收集，避免看起来像没反应
    notifyPlayer(notifyUuid, msg("backup.port.export_start"));
    getLogger().info(msg("log.service.export_start", formatPath(job->batch)));

    ll::thread::ServerThreadExecutor::getDefault().execute([job]() {
        job->records = allPlayers();
        if (job->records.empty()) {
            finishJob(job, msg("backup.command.export_none"));
            return;
        }
        notifyPlayer(job->notifyUuid, msg("backup.port.export_collected", job->records.size()));
        schedulePump(job);
    });
}

void startImportAll(std::string const& notifyUuid, PortDone onDone) {
    if (gJob) {
        notifyPlayer(notifyUuid, msg("backup.port.busy"));
        return;
    }

    auto const batch = currentExportBatch();
    auto       index = batch.empty() ? std::nullopt : readExportIndex();
    if (!index) {
        notifyPlayer(notifyUuid, msg("backup.external.missing_plain"));
        return;
    }

    auto job        = std::make_shared<PortJob>();
    job->kind       = PortKind::Import;
    job->notifyUuid = notifyUuid;
    job->onDone     = std::move(onDone);
    job->batch      = batch;
    job->result.dir = formatPath(batch);
    job->index      = index->value("players", ojson::object());
    for (auto it = job->index.begin(); it != job->index.end(); ++it) {
        job->targets.push_back(it.key());
    }
    if (job->targets.empty()) {
        notifyPlayer(notifyUuid, msg("backup.external.empty"));
        return;
    }
    gJob = job;

    notifyPlayer(notifyUuid, msg("backup.port.import_start", job->targets.size()));
    getLogger().info(msg("log.service.import_start", formatPath(job->batch), job->targets.size()));
    schedulePump(job);
}

bool externalBagsAvailable() { return exportFileExists(); }

std::vector<ExternalBagEntry> listExternalBags() {
    std::vector<ExternalBagEntry> bags;
    auto                          index = readExportIndex();
    if (!index) return bags;

    auto players = index->value("players", ojson::object());
    bags.reserve(players.size());
    for (auto it = players.begin(); it != players.end(); ++it) {
        ExternalBagEntry entry;
        entry.uuid = it.key();
        entry.xuid = it.value().value("xuid", std::string{});
        entry.name = it.value().value("name", std::string{});
        bags.push_back(std::move(entry));
    }
    std::sort(bags.begin(), bags.end(), [](ExternalBagEntry const& a, ExternalBagEntry const& b) {
        return a.name < b.name;
    });
    return bags;
}

std::optional<CompoundTag> readExternalBag(std::string const& uuid) { return readBagFile(bagPathOf(uuid)); }

} // namespace ib
