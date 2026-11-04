#include "mod/core/BackupService.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <system_error>

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
        getLogger().error("还没有任何导出批次（{}）", exportDir().string());
        return std::nullopt;
    }
    auto const content = ll::file_utils::readFile(path);
    if (!content) {
        getLogger().error("找不到导出索引: {}", path.string());
        return std::nullopt;
    }
    try {
        return ojson::parse(*content);
    } catch (std::exception const& e) {
        getLogger().error("导出索引解析失败: {}", e.what());
        return std::nullopt;
    }
}

} // namespace

int64_t backupPlayer(std::string const& uuid, std::string const& note) {
    auto bag = snapshotBag(uuid);
    if (!bag) {
        getLogger().warn("备份失败，取不到背包数据: {}", uuid);
        return 0;
    }
    auto time = backups().create(uuid, *bag, note);
    if (time == 0) {
        getLogger().error("备份写入失败: {}", uuid);
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
        getLogger().warn("查包前缓存自己的背包失败: {}", uuid);
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

// 分片大小：每批处理这么多玩家，然后让出一次服务器线程
constexpr size_t kPortChunk = 8;

// 通知管理员：uuid 空表示只写日志
void notifyPlayer(std::string const& uuid, std::string const& text) {
    if (uuid.empty()) return;
    if (auto* player = onlinePlayer(uuid); player != nullptr) {
        auto packet = TextPacket::createRawMessage(std::string{kMsgPrefix} + text);
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
    int                       lastPct  = 0;
};

std::shared_ptr<PortJob> gJob;

void scheduleChunk(std::shared_ptr<PortJob> job);

void finishJob(std::shared_ptr<PortJob> const& job, std::string const& message) {
    notifyPlayer(job->notifyUuid, message);
    getLogger().info("{}", message);
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

void runExportChunk(std::shared_ptr<PortJob> const& job) {
    auto const total = job->records.size();
    auto const end   = std::min(total, job->pos + kPortChunk);
    for (; job->pos < end; ++job->pos) {
        auto const& record = job->records[job->pos];
        auto        bag    = snapshotBag(record.uuid);
        if (!bag) {
            getLogger().warn("导出时取不到背包数据: {}", record.uuid);
            ++job->result.failed;
            continue;
        }
        if (!writeBagFile(job->batch / (record.uuid + kBagFileSuffix), *bag)) {
            getLogger().warn("导出背包文件写入失败: {}", record.uuid);
            ++job->result.failed;
            continue;
        }
        job->playersJson[record.uuid] = ojson{
            {"xuid", record.xuid                 },
            {"name", record.name                 },
            {"file", record.uuid + kBagFileSuffix},
        };
        ++job->result.success;
    }

    if (job->pos < total) {
        reportProgress(job, total);
        scheduleChunk(job);
        return;
    }

    ojson root{
        {"version",    1                         },
        {"exportedAt", formatTime(nowMs())       },
        {"players",    std::move(job->playersJson)}
    };
    if (!ll::file_utils::writeFile(job->batch / kIndexFileName, root.dump(2))) {
        getLogger().error("导出索引写入失败: {}", (job->batch / kIndexFileName).string());
        job->result.failed += job->result.success;
        job->result.success = 0;
    }
    finishJob(job, msg("backup.stage.export_done", job->result.success, job->result.failed, job->result.dir));
}

void runImportChunk(std::shared_ptr<PortJob> const& job) {
    auto const total = job->targets.size();
    auto const end   = std::min(total, job->pos + kPortChunk);
    for (; job->pos < end; ++job->pos) {
        auto const& uuid = job->targets[job->pos];
        auto        bag  = readBagFile(job->batch / (uuid + kBagFileSuffix));
        if (!bag) {
            getLogger().warn("导入跳过（背包文件缺失或损坏）: {}", uuid);
            ++job->result.failed;
            continue;
        }
        auto const& value = job->index[uuid];
        rememberPlayer(uuid, value.value("xuid", std::string{}), value.value("name", std::string{}));
        std::string error;
        if (restoreToPlayer(uuid, *bag, &error)) {
            ++job->result.success;
        } else {
            getLogger().warn("导入失败 {}: {}", uuid, error);
            ++job->result.failed;
        }
    }

    if (job->pos < total) {
        reportProgress(job, total);
        scheduleChunk(job);
        return;
    }

    savePlayerCache();
    finishJob(job, msg("backup.external.import_all_done", job->result.success, job->result.failed));
}

void runChunk(std::shared_ptr<PortJob> job) {
    if (job->kind == PortKind::Export) {
        runExportChunk(job);
    } else {
        runImportChunk(job);
    }
}

void scheduleChunk(std::shared_ptr<PortJob> job) {
    ll::thread::ServerThreadExecutor::getDefault().execute([job]() { runChunk(job); });
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
        getLogger().error("无法创建导出目录 {}: {}", batch.string(), ec.message());
        notifyPlayer(notifyUuid, msg("backup.error.export_dir", ec.message()));
        return;
    }
    job->batch      = batch;
    job->result.dir = batch.string();
    gJob            = job;

    // 先立刻回一条「开始」，玩家列表放到下一批里收集，避免看起来像没反应
    notifyPlayer(notifyUuid, msg("backup.port.export_start"));
    getLogger().info("开始导出玩家背包到 {}", job->batch.string());

    ll::thread::ServerThreadExecutor::getDefault().execute([job]() {
        job->records = allPlayers();
        if (job->records.empty()) {
            finishJob(job, msg("backup.command.export_none"));
            return;
        }
        notifyPlayer(job->notifyUuid, msg("backup.port.export_collected", job->records.size()));
        scheduleChunk(job);
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
    job->result.dir = batch.string();
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
    getLogger().info("开始从 {} 导入玩家背包（{} 名）", job->batch.string(), job->targets.size());
    scheduleChunk(job);
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
