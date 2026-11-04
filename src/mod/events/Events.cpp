#include "mod/events/Events.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>
#include <utility>

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

int64_t           gLastPurgeDay = 0;
std::thread       gScheduler;
std::atomic<bool> gSchedulerStop{false};

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
            getLogger().warn("玩家退出备份失败: {}", uuid);
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
        getLogger().warn("玩家死亡备份失败: {}", uuid);
    }
}

// 必须在服务端线程执行
void runScheduledWork() {
    Config config = currentConfig();
    auto   now    = nowMs();

    if (config.enableIntervalBackup) {
        int64_t const interval = static_cast<int64_t>(config.backupIntervalMinutes) * 60 * 1000;
        for (auto* player : onlinePlayers(!config.skipSimulatedPlayers)) {
            auto  uuid = player->getUuid().asString();
            auto& last = gLastBackupAt[uuid];
            if (last == 0) {
                last = now;
                continue;
            }
            if (now - last < interval) continue;
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
        if (int removed = backups().purge(config.maxKeepDays); removed > 0) {
            getLogger().info("已清理 {} 条超过 {} 天的背包备份", removed, config.maxKeepDays);
        }
    }
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
        getLogger().error("部分事件监听注册失败，对应的自动备份可能不生效");
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
    gLastPurgeDay = todayStartMs();
    // 这里刻意不用 std::jthread / std::stop_token：MSVC 的 stop_token 会导入
    // MSVCP140_ATOMIC_WAIT.dll（C++20 原子等待），而它只随较新的 vcredist 分发，
    // 老服务器缺这个 DLL 会让模组直接加载失败（错误码 126）。拿 atomic<bool> 顶上。
    gSchedulerStop.store(false, std::memory_order_relaxed);
    gScheduler = std::thread{[] {
        // 启用后先跑一轮，顺便补一次过期清理
        ll::thread::ServerThreadExecutor::getDefault().execute(runScheduledWork);
        while (!gSchedulerStop.load(std::memory_order_relaxed)) {
            for (int i = 0; i < 10 && !gSchedulerStop.load(std::memory_order_relaxed); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            if (gSchedulerStop.load(std::memory_order_relaxed)) break;
            ll::thread::ServerThreadExecutor::getDefault().execute(runScheduledWork);
        }
    }};
}

void stopScheduler() {
    if (gScheduler.joinable()) {
        gSchedulerStop.store(true, std::memory_order_relaxed);
        gScheduler.join();
    }
}

} // namespace ib
