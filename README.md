# InventoryBackup

LeviLamina 服务端模组：玩家背包备份与回档。当前版本 `26.60.0`，适配 LeviLamina `>=26.60.1 <26.70.0`（Minecraft 26.60.6 服务端）。

离线玩家也能备份和回档，不需要本人上线。备份的是背包四件套（背包 / 盔甲 / 副手 / 末影箱），不含经验、血量、坐标、成就等其它数据。

## 功能

- **自动备份**：在线定时备份（默认 15 分钟）、玩家退出备份、玩家死亡备份（可关）
- **按天归档**：备份索引按天分组，可配置保留天数（默认 90 天），跨天自动清理过期备份
- **全玩家查询**：在线玩家、世界库里的所有玩家、按 XboxID / xuid / uuid 模糊搜索；同一个账号的多条映射会合并成一个人
- **查包**：把别人的背包覆盖到自己身上查看，查看前自动缓存自己的背包，可一键恢复
- **回档**：任取一条备份覆盖到目标玩家（在线立即生效并同步世界库；离线直接改库），回档前自动先备份一次
  - 在线回档按物品 NBT 里的 `Slot` 落位（主背包 / 盔甲 / 副手 / 末影箱），不会因为存档列表是稀疏的而错位
  - 需要世界库里已有这个人的记录才允许回档，不会凭空造人
- **暂存**：把当前背包 / 某条备份 / 外部背包收藏成「暂存」，不参与过期清理
- **导入导出**：主菜单一级入口「导入 / 导出」（表单）与 `/inventorybackup export` `/inventorybackup import confirm`（命令）都能用；导出到 `data/export/<批次时间戳>/`，可整包导入或按人挑着用，导出导入都分片执行并报进度
- **删除玩家**：连世界库里的玩家数据一起删（两次确认）

## 安装

把产物目录整个放进服务端的 `mods/InventoryBackup/`（26.60 起 LeviLamina 的模组根目录是 `mods/`，不再是 `plugins/`）。首次启动会生成 `config/config.json` 和 `data/`，两个路径都**相对模组目录**。

- **从 `plugins/` 时代升级**：把 `plugins/InventoryBackup/` 整个目录挪到 `mods/` 下即可，`config/`、`data/` 都是相对模组目录找的，不用改路径
- **换服务器时把 LeviLamina 的 `data/` 一起搬**：玩家名字和 xuid 来自 LL 维护的玩家信息（以及本模组的 `players.json` 名字缓存），只搬 `mods/InventoryBackup/` 的话，「所有玩家」列表里只能显示 uuid 短号、`PlayerInfo` 里也只剩新服进过的人。背包数据本身不受影响，只是不好认人

运行环境：服务端需要 **Microsoft Visual C++ 2015-2022 Redistributable (x64)**，建议装最新版（<https://aka.ms/vs/17/release/vc_redist.x64.exe>）。模组产物只依赖 `LeviLamina.dll` + 系统运行时 —— 也就是说**只要 LeviLamina 能起来，本模组就能加载**。

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

`config/config.json`（首次启动自动生成，当前 `version` 为 3）：

```json
{
    "version": 3,
    "backupIntervalMinutes": 15,
    "maxKeepDays": 90,
    "enableIntervalBackup": true,
    "enableLeaveBackup": true,
    "enableDeathBackup": false,
    "skipSimulatedPlayers": true,
    "pageSize": 10,
    "compressLevel": "default",
    "tickBudgetMs": 5,
    "portChunkSize": 2,
    "indexCacheLimit": 32,
    "backgroundIo": true
}
```

基础项：

- `backupIntervalMinutes`：在线定时备份间隔（分钟，1-1440）
- `maxKeepDays`：备份保留天数（默认 90），设成 `0` 表示永不清理
- `enableIntervalBackup` / `enableLeaveBackup` / `enableDeathBackup`：定时 / 退出 / 死亡备份开关
- `skipSimulatedPlayers`：跳过模拟玩家（NPC）
- `pageSize`：表单每页条数（1-50），玩家列表 / 备份列表 / 暂存列表 / 外部背包列表共用

性能项（都是「省 CPU/内存 ↔ 多占磁盘/多读盘」的旋钮，一般不用动）：

| 键 | 默认 | 说明 |
|---|---|---|
| `compressLevel` | `default` | 备份压缩级别：`fastest` 最快但体积大 11%；`default` 推荐；`best` 体积只小 0.4% 却慢近 5 倍 |
| `tickBudgetMs` | 5 | 定时备份和过期清理**各自**每 tick 最多花多少毫秒，剩下的交给下一 tick（1-50） |
| `portChunkSize` | 2 | 导入导出每批处理几个玩家（1-64）。大批量总耗时约 `人数 / portChunkSize × 50ms` 再加两三个 tick 的固定开销，人多的服调到 4-8；再大收益递减，单 tick 反而变长 |
| `indexCacheLimit` | 32 | 内存里最多缓存多少个玩家的备份索引，超了就清掉一半、下次访问重新读；`0` 表示不限 |
| `backgroundIo` | true | 压缩与读写盘丢给后台线程，不占服务端线程；怀疑并发问题时可以临时关掉，让这些活回到主线程执行 |

配置解析失败时拒绝加载（保持上一次配置），不会覆盖你的文件；改动结构时会留一份 `.bak`，老配置启动时按版本自动补齐新键。

### 推荐配置

默认值本身就是推荐值，绝大多数服用上面那份示例就行。下面两份只列需要调的项，没列出的（`enableXxx`、`skipSimulatedPlayers`、`pageSize` 等）保持默认。

大服（几十人同时在线、备份频繁、盘够大）：

```json
{
    "version": 3,
    "backupIntervalMinutes": 15,
    "maxKeepDays": 30,
    "compressLevel": "default",
    "tickBudgetMs": 3,
    "portChunkSize": 8,
    "indexCacheLimit": 64,
    "backgroundIo": true
}
```

- 保留天数压到 30 天：单个玩家索引从几百 KB 降到几十 KB，写盘量同比减少
- `tickBudgetMs` 降到 3：每 tick 占用更小，代价是重启后集中到期时要多花几秒才追平
- `indexCacheLimit` 放宽到 64：常用玩家的索引不再反复读盘（每人最多几百 KB）
- `portChunkSize` 调到 8：批数越少等 tick 越少，但单 tick 里的开销也越大，别往 64 调

低配（单核 VPS、机械盘、内存紧张）：

```json
{
    "version": 3,
    "backupIntervalMinutes": 30,
    "maxKeepDays": 14,
    "compressLevel": "fastest",
    "tickBudgetMs": 10,
    "portChunkSize": 1,
    "indexCacheLimit": 16,
    "backgroundIo": true
}
```

- `fastest` 换 CPU：体积大 11%，压缩耗时降到 `default` 的六成左右
- `tickBudgetMs` 放到 10：机械盘写盘抖动时不至于把 tick 拖太久
- `indexCacheLimit` 收到 16：内存优先，代价是被踢出的玩家再查包要重读一次索引

## 数据目录

`data/` 相对模组目录，即 `<服务端>/mods/InventoryBackup/data/`：

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
- **过期清理只管备份包**：`maxKeepDays` 清理的是 `backups/`，`export/<时间戳>/` 下的导出批次不会被自动删除，攒多了要自己清
- 老布局（`data/index.json`、`data/staged.json`、摊平的 `export/`）启动时会自动迁移，原文件改名成 `*.migrated-*` / 归入 `export/<时间戳>/`

背包文件可以直接用 NBT 工具查看，里面只有背包 / 盔甲 / 副手 / 末影箱四个标签。

## 多语言

界面文字跟随玩家自己的语言（中文 / 英文），控制台日志走服务端默认语言。要改文案就直接编辑 `lang/zh_CN.json` 或 `lang/en_US.json`，两边的键一一对应。

## 构建

```bash
xmake f -y -p windows -a x64 -m release
xmake
```

产物在 `bin/InventoryBackup/`，版本号取自 `tooth.json`。

开发相关的实现说明（世界库键格式、线程模型、版本号生成、源码目录）见 [DEVELOPMENT.md](DEVELOPMENT.md)，改动历史见 [CHANGELOG.md](CHANGELOG.md)。

## 许可证

[GNU AGPL v3](LICENSE)。二次开发后对外提供在线服务时，按 AGPL 的要求需要向使用者提供修改后的源码。
