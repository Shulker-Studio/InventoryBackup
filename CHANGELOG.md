# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/).

版本号与所适配的 LeviLamina 对齐（`26.51.x`），当前 `26.51.1` 适配 LeviLamina 26.51.9。

## [26.51.1] - 2026-11-05

### Fixed

- 修复部分服务端（vcredist 偏旧）启用时报 `std::system_error` / `[0x0000007E:system]` 无法加载的问题：时区改用 CRT 的 `localtime_s`，定时线程改用 `std::thread` + `std::atomic<bool>`，产物不再依赖 `MSVCP140_ATOMIC_WAIT.dll`，只要 LeviLamina 能启动就能加载

## [26.51.0] - 2026-11-04

首个版本：把原 JS 插件 WnBagBackup 的功能用原生 C++ 重写，**不依赖 GMLIB**。

### Added

- **自动备份**：在线定时（`backupIntervalMinutes`，默认 15）、玩家退出、玩家死亡（默认关，`enableDeathBackup` 打开）三类触发，外加 `/inventorybackup online` 手动备份全体在线玩家
- **按天归档**：备份按天分组，保留 `maxKeepDays` 天（默认 90，0 表示永不清理），跨天自动清理过期备份与文件；备份可加/改备注
- **离线玩家背包读写**：直接用 BDS `LevelStorage`。先在 `player_<uuid>` 映射记录里取 `ServerId`，再用它去拿 `player_server_<id>` 的实体 NBT —— 已验证真机 1876 条映射全部指向存在的数据键
- **查包与回档**：在线玩家回档立即生效（逐格写回主背包 / 四格盔甲 / 副手 / 末影箱并同步客户端，同时写回世界库），离线玩家直接写世界库
- **查包前缓存**：查看别人背包前先缓存自己那份，回档后可一键「恢复背包到上次查包前」
- **暂存背包**：把某人的当前背包暂存下来随时取用，不参与过期清理
- **导入导出**：导出全体玩家背包到 `data/export/<批次时间戳>/`，支持整包导入或按人挑用
- **管理表单**：在线玩家、全部玩家、模糊搜索（名字 / xboxid / uuid）、玩家操作、按天查备份、导出导入，分页带跳转
- **命令**：`/inventorybackup`（别名 `/ib`，GameDirectors 权限），子命令 `online`、`reload`、`export`、`import`、`import confirm`
- **配置版本化**：`version` 字段驱动的逐版本迁移（当前 v2），迁移前自动留 `config.json.bak`；表单每页条数 `pageSize`（1~50，默认 10）
- **多语言**：`lang/zh_CN.json`、`lang/en_US.json`，表单文案跟随执行者游戏语言

### Changed

- **数据布局 `dataVersion = 2`**：备份索引改成每个玩家一个 `backups/<uuid>/index.json`（懒加载，损坏只影响单个玩家），查包缓存改成 `cache/<查看者uuid>/`，导出改成批次目录 `export/<时间戳>/`；启动时自动迁移旧布局，原文件改名归档而不是直接删除
- **导出 / 导入改为分片执行**：每批 8 名玩家让出一次线程，开始、进度（每 10%）、完成都有聊天栏提示，管理员不会再遇到「点了导出毫无反应」
- 备份文件为小端二进制 NBT 经 gzip 压缩的 `.nbt.gz`，只保留 `Inventory`、`Armor`、`Offhand`、`EnderChestInventory` 四个标签

### Fixed

- 离线读取取错键：早期版本按 `player_<serverId>` 读，拿到的是 113~166 字节的身份映射记录（没有 `Inventory`），备份必然失败；现按映射记录的 `ServerId` 定位实体数据键
- 回档按 `Slot` 标签逐件落位，不再假设列表下标等于背包槽位（Bedrock 存档只存非空物品且自带 `Slot`），盔甲 `Slot 0~3` 映射到头/胸/腿/脚
- 查包缓存原本是全局单槽，管理员 A 的恢复点会被管理员 B 的查包动作覆盖；现按管理员 uuid 隔离
- 表格文案统一走 `ll::i18n`，不再有硬编码中文泄漏到玩家界面

### Known limitations

- 真机世界库里有 952 个 `ServerId`，其中 924 个被两个以上 uuid 共用（BDS 的映射不唯一），往这种共享键做离线回档可能同时影响多名玩家，操作前请确认
- 与旧 JS 插件的导出格式不兼容（旧插件是单个 `data/export_bagdata.json` + base64），LevelDB 存储也不做迁移，新插件从空历史开始
- 离线玩家的血量 / 位置 / 经验等状态虽然能从同一份实体 NBT 读到，但目前没有对外界面
