#include "mod/events/Events.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ll/api/coro/CoroTask.h"
#include "ll/api/event/EventBus.h"
#include "ll/api/event/player/PlayerDieEvent.h"
#include "ll/api/event/player/PlayerDisconnectEvent.h"
#include "ll/api/event/player/PlayerJoinEvent.h"
#include "ll/api/thread/ServerThreadExecutor.h"
#include "mc/world/actor/player/Player.h"
#include "mod/core/BackupService.h"
#include "mod/core/BackupStore.h"
#include "mod/i18n/Text.h"
#include "mod/Mod.h"
#include "mod/core/PlayerData.h"
#include "mod/core/Util.h"

namespace ib {

namespace {

ll::event::ListenerPtr gJoinListener;
ll::event::ListenerPtr gDisconnectListener;
ll::event::ListenerPtr gDieListener;

// uuid -> 上次备份时间（毫秒）
std::unordered_map<std::string, int64_t> gLastBackupAt;

// 空闲心跳：定时备份是分钟粒度，没必要 5 秒一醒。
// 取「备份间隔 / 4」并夹在 5~30 秒，1 分钟的配置也准时，而 15 分钟的默认配置只要 30 秒一醒。
constexpr std::chrono::milliseconds kMinIdleTick{5000};
constexpr std::chrono::milliseconds kMaxIdleTick{30000};
// 还有活没干完时的心跳：缩到 1 tick（50ms），尽快把积压的做完再回到空闲心跳
constexpr std::chrono::milliseconds kBusyTick{50};

int64_t gLastPurgeDay = 0;
// 上一拍备份没做完（超预算），下一拍缩短心跳接着做
bool gBackupPending = false;
// 分片清理的进度（当天跑完后清空）
std::vector<std::string> gPurgeUuids;
size_t                   gPurgeCursor  = 0;
size_t                   gPurgeRemoved = 0;

// 停止标志：每一轮调度拿一个新的 token，旧循环醒了看到自己的 token 就退出
std::shared_ptr<std::atomic<bool>> gSchedulerStop;

bool purgePending() { return gPurgeCursor < gPurgeUuids.size(); }

std::chrono::milliseconds idleTick() {
    auto const interval = static_cast<int64_t>(currentConfig().backupIntervalMinutes) * 60000;
    auto const quarter  = std::chrono::milliseconds{interval / 4};
    return std::clamp(quarter, kMinIdleTick, kMaxIdleTick);
}

void onPlayerJoin(ll::event::player::PlayerJoinEvent& event) {
    auto& player = event.self();
    auto  uuid   = player.getUuid().asString();
    rememberPlayer(uuid, player.getXuid(), player.getRealName());
    gLastBackupAt.insert_or_assign(uuid, nowMs());

    // 顺带捕获一份空玩家模板（用于给「从没进过服 / 被删档」的玩家建档）
    if (!hasPlayerTemplate()) {
        if (auto live = readLiveTag(player); live) capturePlayerTemplate(*live);
    }
}

void onPlayerDisconnect(ll::event::player::PlayerDisconnectEvent& event) {
    auto& config = Mod::getInstance().config();
    auto& player = event.self();
    auto  uuid   = player.getUuid().asString();

    rememberPlayer(uuid, player.getXuid(), player.getRealName());
    if (config.value_or(Config{}).enableLeaveBackup) {
        if (backupPlayer(uuid, msg("backup.note.leave")) == 0) {
            getLogger().warn(msg("log.event.leave_backup_failed", uuid));
        }
    }
    gLastBackupAt.erase(uuid);
    savePlayerCache();
}

void onPlayerDie(ll::event::player::PlayerDieEvent& event) {
    auto& config = Mod::getInstance().config();
    if (!config.value_or(Config{}).enableDeathBackup) return;

    auto& player = event.self();
    if (isSimulated(player)) return;
    auto uuid = player.getUuid().asString();
    if (backupPlayer(uuid, msg("backup.note.death")) == 0) {
        getLogger().warn(msg("log.event.death_backup_failed", uuid));
    }
}

// 必须在服务端线程执行
void runScheduledWork() {
    Config config = currentConfig();
    auto   now    = nowMs();
    // 每拍最多占服务端线程多久：备份和清理共用这个预算，可以配（config.tickBudgetMs）
    auto const budget = std::chrono::milliseconds{config.tickBudgetMs};

    gBackupPending = false;

    if (config.enableIntervalBackup) {
        int64_t const interval = static_cast<int64_t>(config.backupIntervalMinutes) * 60 * 1000;
        auto const    begin    = std::chrono::steady_clock::now();
        int           done     = 0;
        for (auto* player : onlinePlayers(!config.skipSimulatedPlayers)) {
            auto  uuid = player->getUuid().asString();
            auto& last = gLastBackupAt[uuid];
            if (last == 0) {
                last = now;
                continue;
            }
            if (now - last < interval) continue;
            // 一人一拍的标准上很便宜，但重启后大家同时到期，一次几十个就是一两百毫秒的尖峰；
            // 超预算先收手，没处理的 last 没被推进，下一拍自然接着来
            if (done > 0 && std::chrono::steady_clock::now() - begin > budget) {
                gBackupPending = true;
                break;
            }
            ++done;
            if (backupPlayer(uuid, msg("backup.note.interval")) != 0) {
                last = now;
            } else {
                // 失败时推迟一分钟再试，避免每轮都刷失败日志
                last = now - interval + 60 * 1000;
            }
        }
    }

    int64_t const today = todayStartMs();
    if (gLastPurgeDay != today) {
        gLastPurgeDay = today;
        gPurgeUuids   = backups().indexedUuids();
        gPurgeCursor  = 0;
        gPurgeRemoved = 0;
    }
    if (purgePending()) {
        gPurgeRemoved += backups().purgeBatch(gPurgeUuids, gPurgeCursor, config.maxKeepDays, budget);
        if (!purgePending()) {
            if (gPurgeRemoved > 0) {
                getLogger().info(msg("log.purge_done", gPurgeRemoved, config.maxKeepDays));
            }
            gPurgeUuids.clear();
            gPurgeUuids.shrink_to_fit();
            gPurgeRemoved = 0;
        }
    }

    // 收尾：索引缓存有上限，满过一次就清到一半（这里没有任何人持有 indexOf 的引用）
    backups().trimIndexCache(static_cast<size_t>(config.indexCacheLimit));
}

} // namespace

void registerEvents() {
    auto& bus = ll::event::EventBus::getInstance();
    if (!gJoinListener) {
        gJoinListener = bus.emplaceListener<ll::event::player::PlayerJoinEvent>(onPlayerJoin);
    }
    if (!gDisconnectListener) {
        gDisconnectListener = bus.emplaceListener<ll::event::player::PlayerDisconnectEvent>(onPlayerDisconnect);
    }
    if (!gDieListener) {
        gDieListener = bus.emplaceListener<ll::event::player::PlayerDieEvent>(onPlayerDie);
    }
    if (!gJoinListener || !gDisconnectListener || !gDieListener) {
        getLogger().error(msg("log.event.listen_failed"));
    }
}

void unregisterEvents() {
    auto& bus = ll::event::EventBus::getInstance();
    if (gDieListener) {
        bus.removeListener<ll::event::player::PlayerDieEvent>(gDieListener);
        gDieListener.reset();
    }
    if (gDisconnectListener) {
        bus.removeListener<ll::event::player::PlayerDisconnectEvent>(gDisconnectListener);
        gDisconnectListener.reset();
    }
    if (gJoinListener) {
        bus.removeListener<ll::event::player::PlayerJoinEvent>(gJoinListener);
        gJoinListener.reset();
    }
    gLastBackupAt.clear();
}

void startScheduler() {
    stopScheduler();
    // 从 0 开始：启动后第一拍补一次过期清理（以前预置成今天，跨天关机的服永远清不到）
    gLastPurgeDay  = 0;
    gBackupPending = false;
    gPurgeUuids.clear();
    gPurgeCursor  = 0;
    gPurgeRemoved = 0;

    auto token     = std::make_shared<std::atomic<bool>>(false);
    gSchedulerStop = token;

    // 用 ll 的协程 + 服务端线程执行器，不再自己开线程：co_await 一个 duration 就是
    // executeAfter（按 tick 计时），到期在服务端线程上恢复，世界不 tick 时自然不跑
    ll::coro::keepThis([token]() -> ll::coro::CoroTask<> {
        while (!token->load(std::memory_order_relaxed)) {
            co_await (purgePending() || gBackupPending ? kBusyTick : idleTick());
            if (token->load(std::memory_order_relaxed)) co_return;
            runScheduledWork();
        }
        co_return;
    }).launch(ll::thread::ServerThreadExecutor::getDefault());
}

void stopScheduler() {
    // 只置标志，不销毁协程帧：SleepAwaiter 把 handle 留在了执行器里，
    // 销毁帧会让那一拍去恢复已释放的帧。循环下一拍醒来自己退出，帧随之释放
    if (gSchedulerStop) gSchedulerStop->store(true, std::memory_order_relaxed);
    gSchedulerStop.reset();

    gBackupPending = false;
    gPurgeUuids.clear();
    gPurgeCursor  = 0;
    gPurgeRemoved = 0;
}

} // namespace ib
