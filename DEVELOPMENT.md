# DEVELOPMENT

实现说明与代码约定。使用者只需要看 [README.md](README.md)。

## 源码目录

```
src/mod/
  Mod.cpp / Mod.h            模组生命周期（load/enable/disable）
  MemoryOperators.cpp        接管内存分配器（用 LeviLamina 的）
  Version.h.in               构建期生成的版本头
  InventoryBackup.rc         Windows 版本资源
  command/Command.cpp        命令注册
  config/Config.cpp          配置读写与校验
  config/ConfigMigration.cpp 配置版本迁移（不依赖 LeviLamina，可单独编译自测）
  events/Events.cpp          事件监听 + 协程调度（定时备份 / 过期清理）
  core/PlayerData.cpp        世界库玩家 NBT 读写（在线/离线）
  core/BagOps.cpp            背包抽取与回档（在线立即生效）
  core/BackupStore.cpp       备份文件 + 索引 + 暂存 + 查包缓存
  core/BackupService.cpp     备份/回档/导入导出编排
  core/AsyncIo.cpp           后台 I/O 派发（线程池 ↔ 服务端线程）
  core/Util.cpp              时间、字符串、路径工具
  i18n/Text.cpp              ll::i18n 薄封装（载入 lang/、按玩家语言取文案）
  form/FormCommon.cpp        消息、分页列表、确认框
  form/MainMenu.cpp          主菜单 / 搜索
  form/PlayerForms.cpp       玩家功能 / 备份浏览
  form/StagedForms.cpp       暂存 / 外部背包 / 导入导出
```

仓库根目录：`xmake.lua`（构建脚本）、`tooth.json`（版本与打包信息）、`lang/`（翻译）、`CHANGELOG.md`。

## 世界库键格式

离线玩家背包直接读写 BDS 世界库（LevelDB）的玩家数据，用的是原版自己的键：

- `player_<uuid>`：映射记录，值是一段 NBT —— `{ MsaId, SelfSignedId, ServerId }`
- `player_server_<id>`：玩家实体 NBT，`ServerId` 标签的值就是这个完整键名；`Inventory` / `Armor` / `Offhand` / `EnderChestInventory` 都在这里

数据键不是拼出来的，而是从映射记录的 `ServerId` 里取（权威来源）。`dataKeyOf(uuid)` 的解析顺序：

1. `loadData("player_" + uuid)` 读映射，取 NBT 里的 `ServerId`（旧存档里也可能是裸字符串）
2. 在线玩家直接问 `LevelStorage::getServerId(player, true)`
3. 兼容探测历史世界的 `player_server_<uuid>` 键

`ServerId` 与 `uuid` 之间没有可推导关系，唯一映射就存在第 1 步的 NBT 里 —— 所以枚举玩家必须从映射键出发，不能只枚举 `player_server_*`（那样既拿不到 uuid，枚举本身也不完整：真实存档上 `player_server` 前缀只枚举到 953 条，而 `hasKey` 逐条探到 1876 条都在）。

三条踩过的坑：

- **`forEachKeyWithPrefix` 回调里拿到的键已经剥掉了匹配到的前缀**。以 `player_` 为前缀枚举时，映射键是 `<uuid>`、数据键是 `server_<id>`。早期版本在回调里又检查了一次 `player_` 前缀，导致一条键都认不出来、`storedUuids()` 恒返回空（旧存档上表现为「所有玩家只显示 2 个」）。现在两种形态都认，扫到键却一条都认不出来会告警
- **同一个账号可能有多条映射**：离线 uuid 和 Xbox uuid 各一条，指向同一个 `player_server_<id>`。`allPlayers()` 按数据键去重、优先保留有名字的那条 uuid（真实存档 1876 条映射去重后 952 人）
- `LevelStorage::loadAllPlayerIDs` / `createWriteBatch` 在 LeviLamina 26.60 是 `MCNAPI`（不导出），用不了；`getServerId` 在 26.60.0 被标成 `MCNAPI`、26.60.1 才恢复 `MCAPI`，所以 `tooth.json` 卡 `>=26.60.1`

## 线程模型

按操作边界切分，绝不把 BDS/LeviLamina 的线程亲和对象带进后台线程。

必须在服务端线程：`Player::save`、`readLiveTag`、`snapshotBag`、`restoreToPlayer`、`writeStoredTag`、世界库读写、`mIndex` 与 job 状态、表单、`msg()`、日志、索引提交。

可以丢线程池：gzip 压缩/解压、索引 JSON 序列化/解析、大文件读写、目录枚举、过期文件删除、导出包写盘、导入读取+解压。

实现落点：

- `core/AsyncIo.h` 的 `runIo(fn)` 在 `config.backgroundIo` 为真时投到 `ThreadPoolExecutor::getDefault()`，否则就地执行；`backToServer(fn)` 用 `ServerThreadExecutor::getDefault()` 回到服务端线程。后台任务只能携带不可变数据（uuid、时间戳、字节串、错误码），不得碰玩家/世界库/`mIndex`/`msg()`/表单/日志
- `bagio`（`BackupStore.h`）按线程亲和拆开：`encode` / `decode` 会物化 NBT 对象，只能在服务端线程；`deflate` / `inflate` / `readBinary` / `writeBinary` 只处理字节串，可以进池子。压缩档位由服务端线程用 `levelFrom(config.compressLevel)` 算好后按值传进去，避免后台线程读配置产生竞争
- `BackupStore::create()` 是乐观的：先在内存里占位（UI 立刻可见）、返回时间戳表示"已受理"，`runIo` 里压缩写盘，`backToServer` 里 `finishCreate` 再提交索引；索引写失败会 `rollbackCreate` 撤掉这条备份。占位但还没落盘的条目记在 `mPendingWrites` 里，`daysToJson` 不序列化它们，`trimIndexCache` / `purgeOne` 也不驱逐这些 uuid
- 覆盖类操作（覆盖别人背包、回档、应用外部背包）前要先有一份**已确认写盘**的保险备份，所以它们走 `backupPlayerSync` / `createSync`，不用乐观路径
- 调度器是 `ll::coro::keepThis` 的协程循环（`events/Events.cpp`），不再有常驻线程；等不到活时按 `backupIntervalMinutes/4` 自适应 5-30 秒醒来一次。过期清理按 `tickBudgetMs` 分片（`purgeBatch`），导出导入按 `portChunkSize` 控制同时在飞的任务数

## 版本号

版本号只写一次：`tooth.json` 的 `version`。`xmake.lua` 的 `on_load` 读它、校验 `x.y.z` 格式、拼上 `git rev-parse --short HEAD` 作为构建元数据（`26.60.0+9446ed8`），再注入 `mod/Version.h`（由 `Version.h.in` 生成）、`mod/InventoryBackup.rc` 的版本资源，并传给 `modpacker` 写进 `manifest.json`。

## 多语言

`ll::i18n` 的薄封装在 `i18n/Text.h`：`msg(key, args...)` 走默认语言（控制台日志、配置错误），`msgFor(locale, key, ...)` / `textFor(player)` 跟随玩家语言（表单、命令输出、聊天提示）。所有 key 都在 `lang/zh_CN.json` 与 `lang/en_US.json`，两边必须一一对应、`{}` 占位符个数一致。

两处约定：

- 日志一律 `getLogger().level(msg(key, args...))`，不要自己拼字符串
- `ConfigMigration.cpp` 刻意不依赖 LeviLamina，所以它不能调 `msg()`；校验只返回 `config.error.*` 的 key，由 `Config.cpp` 拼上 detail（如 `e.what()`）再取文案

## 更新日志

`CHANGELOG.md` 按 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.0.0/) 组织，分类只用 `Added` / `Changed` / `Fixed` / `Removed` / `Security`，没内容的小节不要留空标题。约束是为了别让它变成提交日志的复印件：

1. **新改动一律写进顶部的 `## [Unreleased]`**，发布时才改名为 `## [x.y.z] - 日期`，并新开一个空的 `[Unreleased]`
2. **只写"服主能感知的差异"**：一条一句、不换行、尽量 80 字以内。函数名、DB 键名、线程与存储细节属于这里（DEVELOPMENT.md）或提交信息，不进 CHANGELOG
3. **同一节里同一条改动只写一次**。并行的发布线（`26.51.x` 与 `26.60.x`）各自都要包含这条修复时，两边都写并注明「同步自 x.y.z」
4. **纯内部整理不写日志**：拆文件、改注释、加日志、补校验都不写；重构只有带来可感知的结果（如"界面支持英文了"）才写
5. **发布时一起做**：`tooth.json` 升版本 → `[Unreleased]` 改名并填日期 → `xmake` 确认产物版本号 → 打 tag
6. **文末维护版本链接块**（Keep a Changelog 的要求）：最早一个版本指向 `releases/tag/<tag>`，之后的版本用 `compare/<上一版>...<本版>`，`[Unreleased]` 指向 `compare/<最新 tag>...HEAD`。发布时把 `[Unreleased]` 的基线改成本次 tag

**发版待办**：下一版定为 `26.60.1`。`tooth.json` 现在还是 `26.60.0`（对应已发布的 v26.60.0），发版时一起做四件事：把它升到 `26.60.1`、把 README 的「当前版本」同步、把 `[Unreleased]` 改名为 `## [26.60.1] - 日期`、重建确认产物版本后打 tag。在那之前不要用当前 `tooth.json` 构建对外发布的包。

## 代码约定

- 源码不写死自然语言（除注释），一律走 `lang/`；不翻译命令名、文件名、JSON/NBT 字段名、`§` 颜色码
- **不要对这个仓库跑 clang-format**：现有代码是旧版 clang-format 的产物，本机的 20+ 版本复现不出（会把参数合并到一行、重排 include），会产生大量无关 diff。`Config.h` 字段块、`BackupStore.h` 的 `bagio` 注释列都是手工对齐的
- `ConfigMigration.cpp` 保持依赖精简（只有 nlohmann/json），新增配置项要同时改：`Config.h` 的默认值与范围注释、`toJson`、解析块、`migrateStep` 的补齐分支、`validateConfig` 的范围校验，以及两个 lang 文件的 `config.error.*`
