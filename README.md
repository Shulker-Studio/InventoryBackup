# InventoryBackup

LeviLamina 服务端模组：玩家背包备份与回档。当前版本 `26.51.1`，适配 LeviLamina 26.51.9。版本号由构建时的 git tag 决定（`v26.51.1` → `26.51.1`），跟 LeviLamina 版本对齐；改动见 [CHANGELOG.md](CHANGELOG.md)。

功能对齐 `WnBagBackup`（LSE 脚本版），但不依赖 GMLIB / LegacyRemoteCall —— 离线玩家背包直接读写 **BDS 世界库（LevelDB）** 里的玩家数据，用的是原版自己的键（已用真实世界库核查，见 `tools/verify_player_key_scheme.py`）：

- `player_<uuid>`：映射记录，值是一段 NBT —— `{ MsaId, SelfSignedId, ServerId }`
- `player_server_<id>`：玩家实体 NBT，`ServerId` 标签的值就是这个完整键名；`Inventory` / `Armor` / `Offhand` / `EnderChestInventory` 都在这里

所以数据键不是自己拼出来的，而是从映射记录的 `ServerId` 里取；在线玩家则直接问 `LevelStorage::getServerId()`。

## 功能

- **自动备份**：在线定时备份（默认 15 分钟）、玩家退出备份、玩家死亡备份（可关）
- **按天归档**：备份索引按天分组，可配置保留天数（默认 90 天），每天跨天自动清理过期备份
- **全玩家查询**：在线玩家、世界库里的所有玩家、按 XboxID / xuid / uuid 模糊搜索
- **查包**：把别人的背包覆盖到自己身上查看，查看前自动缓存自己的背包，可一键恢复
- **回档**：任取一条备份覆盖到目标玩家（在线立即生效并同步世界库；离线直接改库），回档前自动先备份一次
  - 在线回档按物品 NBT 里的 `Slot` 落位（主背包 / 盔甲 / 副手 / 末影箱），不会因为存档列表是稀疏的而错位
  - 只有世界库里已有映射记录（`player_<uuid>`）的玩家才允许回档；数据键一律取自 BDS 自己的记录，不会凭空生成
- **暂存**：把当前背包 / 某条备份 / 外部背包收藏成「暂存」，不参与过期清理
- **导入导出**：主菜单一级入口「导入 / 导出」（表单）与 `/inventorybackup export` `/inventorybackup import confirm`（命令）都能用；导出到 `data/export/<批次时间戳>/`，可整包导入或按人挑着用，导出导入都分片执行并报进度
- **删除玩家**：连世界库里的玩家 NBT 一起删（两次确认）

## 命令

```
/inventorybackup  (或 /ib)     打开管理表单（玩家执行）
/inventorybackup online        立刻备份所有在线玩家
/inventorybackup reload        重载配置 / 备份索引 / 暂存索引
/inventorybackup export        导出所有玩家背包到 data/export/<时间戳>/（分片执行，聊天栏报进度）
/inventorybackup import        提示加 confirm（导入会覆盖所有玩家）
/inventorybackup import confirm 执行导入
```

权限等级：`GameDirectors`（OP）。

## 配置

`config/config.json`（首次启动自动生成）：

```json
{
    "version": 1,
    "backupIntervalMinutes": 15,
    "maxKeepDays": 90,
    "enableIntervalBackup": true,
    "enableLeaveBackup": true,
    "enableDeathBackup": false,
    "skipSimulatedPlayers": true,
    "pageSize": 10
}
```

- `maxKeepDays` 设为 `0` 表示永不清理
- `pageSize` 是表单每页条数（1-50），玩家列表 / 备份列表 / 暂存列表 / 外部背包列表共用
- 配置解析失败时拒绝加载（保持上一次配置），不会覆盖你的文件；改动结构时会留一份 `.bak`

## 数据目录

```
data/
  store.json                        持久化布局版本（当前 dataVersion = 2）
  players.json                      uuid -> {xuid, name} 名字缓存
  backups/<uuid>/index.json         该玩家的备份索引（按天分组，懒加载）
  backups/<uuid>/<时间>.nbt.gz      一次备份 = 一份背包 NBT（gzip）
  staged/index.json                 暂存索引
  staged/<时间>.nbt.gz              暂存背包
  cache/<查看者uuid>/meta.json      查包前缓存的时间/来源
  cache/<查看者uuid>/bag.nbt.gz     查包前缓存的自己背包
  export/<批次时间戳>/index.json    一次导出 = 一个批次目录
  export/<批次时间戳>/<uuid>.nbt.gz
```

- **每个玩家一个索引**：加一条备份只重写那个玩家的小文件，玩家多了也不会互相拖慢；单个索引损坏只影响一个人（损坏文件会改名成 `index.json.broken-<时间>`）
- **查包缓存按管理员隔离**：`cache/<uuid>/` 各存各的，两个管理员同时查包不会互相覆盖恢复点
- **导出按批次归档**：每次导出都新建 `export/<时间戳>/`，不覆盖以前的批次；「查看外部背包」用的是最新批次
- 老布局（`data/index.json`、`data/staged.json`、摊平的 `export/`）启动时会自动迁移，原文件改名成 `*.migrated-*` / 归入 `export/<时间戳>/`

背包文件就是小端二进制 NBT，只有四个标签：`Inventory`、`Armor`、`Offhand`、`EnderChestInventory`，可以直接用 NBT 工具查看。

## 多语言

`lang/zh_CN.json`、`lang/en_US.json`。表单文案跟随执行命令的玩家语言（`ll::i18n` + `Player::getLocaleCode()`），缺 key 时显示 key 本身（便于发现漏翻）。

## 构建

```bash
xmake f -y -p windows -a x64 -m release
xmake
```

产物在 `bin/InventoryBackup/`。

安装：把产物目录放进服务端的 `plugins/InventoryBackup/`。

运行环境：服务端需要 **Microsoft Visual C++ 2015-2022 Redistributable (x64)**，建议装最新版（<https://aka.ms/vs/17/release/vc_redist.x64.exe>）。模组产物的导入表只有 `LeviLamina.dll`、`MSVCP140.dll`、`VCRUNTIME140.dll`、`VCRUNTIME140_1.dll` 和系统 UCRT —— 也就是说**只要 LeviLamina 能起来，本模组就能加载**；`26.51.0` 额外依赖的 `MSVCP140_ATOMIC_WAIT.dll`（时区库 / `std::jthread`）已在 `26.51.1` 去掉。

## 目录结构

```
src/mod/
  Mod.cpp                 模组生命周期（load/enable/disable）
  command/                命令注册
  events/                 事件监听 + 定时任务
  config/                 配置读写
  core/                   核心读写
    PlayerData.cpp        世界库玩家 NBT 读写（在线/离线）
    BagOps.cpp            背包抽取与回档（在线立即生效）
    BackupStore.cpp       备份文件 + 索引 + 暂存 + 查包缓存
    BackupService.cpp     备份/回档/导入导出编排
    Util.cpp              时间、字符串工具
  i18n/                   ll::i18n 的薄封装（载入 lang/、按玩家语言取文案）
  form/                   表单界面
    FormCommon.cpp        消息、分页列表、确认框
    MainMenu.cpp          主菜单 / 搜索
    PlayerForms.cpp       玩家功能 / 备份浏览
    StagedForms.cpp       暂存 / 外部背包 / 导入导出
```
