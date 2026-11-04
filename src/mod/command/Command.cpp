#include "mod/command/Command.h"

#include <string>

#include "ll/api/command/CommandHandle.h"
#include "ll/api/command/CommandRegistrar.h"
#include "mc/server/commands/CommandOrigin.h"
#include "mc/server/commands/CommandOutput.h"
#include "mc/world/actor/Actor.h"
#include "mc/world/actor/player/Player.h"
#include "mod/core/BackupService.h"
#include "mod/core/BackupStore.h"
#include "mod/core/PlayerData.h"
#include "mod/i18n/Text.h"
#include "mod/Mod.h"
#include "mod/form/MainMenu.h"

namespace ib {

namespace {

std::string switchLabel(bool enabled) { return msg(enabled ? "backup.command.switch_on" : "backup.command.switch_off"); }

} // namespace

void registerCommand() {
    static bool registered = false;
    if (registered) return;
    registered = true;

    auto& command = ll::command::CommandRegistrar::getInstance(/*isClientSide=*/false)
                        .getOrCreateCommand(
                            "inventorybackup",
                            msg("backup.command.desc"),
                            CommandPermissionLevel::GameDirectors
                        );
    command.alias("ib");

    command.overload().text("online").execute([](CommandOrigin const&, CommandOutput& output) {
        int const count = backupOnlinePlayers(msg("backup.note.console"));
        if (count > 0) {
            output.success("{}", msg("backup.command.online_ok", count));
        } else {
            output.error("{}", msg("backup.command.online_none"));
        }
    });

    command.overload().text("reload").execute([](CommandOrigin const&, CommandOutput& output) {
        if (auto const error = Mod::getInstance().reloadConfig(); !error.empty()) {
            output.error("{}", error);
            return;
        }
        auto const data = Mod::getInstance().dataDir();
        backups().load(data);
        staged().load(data);
        savePlayerCache();

        auto const config = currentConfig();
        output.success(
            "{}",
            msg(
                "backup.command.reload_ok",
                switchLabel(config.enableIntervalBackup),
                config.backupIntervalMinutes,
                switchLabel(config.enableLeaveBackup),
                switchLabel(config.enableDeathBackup),
                config.maxKeepDays
            )
        );
    });

    command.overload().text("export").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (portBusy()) {
            output.error("{}", msg("backup.port.busy"));
            return;
        }
        auto*       entity = origin.getEntity();
        std::string uuid   = (entity != nullptr && entity->isPlayer()) ? static_cast<Player*>(entity)->getUuid().asString() : std::string{};
        startExportAll(uuid, [](PortResult const& result) {
            getLogger().info("导出完成：成功 {} / 失败 {}，目录 {}", result.success, result.failed, result.dir);
        });
        output.success("{}", msg("backup.port.export_start"));
    });

    // 导入会覆盖所有玩家当前背包，必须显式加 confirm
    command.overload().text("import").execute([](CommandOrigin const&, CommandOutput& output) {
        output.error("{}", msg("backup.command.import_hint"));
    });
    command.overload().text("import").text("confirm").execute([](CommandOrigin const& origin, CommandOutput& output) {
        if (!externalBagsAvailable()) {
            output.error("{}", msg("backup.external.missing_plain"));
            return;
        }
        if (portBusy()) {
            output.error("{}", msg("backup.port.busy"));
            return;
        }
        auto*       entity = origin.getEntity();
        std::string uuid   = (entity != nullptr && entity->isPlayer()) ? static_cast<Player*>(entity)->getUuid().asString() : std::string{};
        startImportAll(uuid, [](PortResult const& result) {
            getLogger().info("导入完成：成功 {} / 失败 {}", result.success, result.failed);
        });
        output.success("{}", msg("backup.command.import_ok"));
    });

    command.overload().execute([](CommandOrigin const& origin, CommandOutput& output) {
        auto* entity = origin.getEntity();
        if (entity == nullptr || !entity->isPlayer()) {
            output.error("{}", msg("backup.command.player_only"));
            return;
        }
        openMainMenu(*static_cast<Player*>(entity));
        output.success("");
    });
}

} // namespace ib
