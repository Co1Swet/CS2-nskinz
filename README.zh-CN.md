[![nSkinzCS2 HLAE 离线 Demo 皮肤工具](./docs/assets/nskinz-banner.svg)](./docs/assets/nskinz-banner.svg)

<h3 align="center">面向 HLAE Demo 回放的 Counter-Strike 2 离线外观工具</h3>

地图资源诊断使用了 [Source 2 Viewer](https://s2v.app)（[ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat)）辅助验证。

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/blob/main/README.md"><img src="https://img.shields.io/badge/English-README-555?style=flat-square" alt="English README"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/blob/main/README.zh-CN.md"><img src="https://img.shields.io/badge/简体中文-README-1f6feb?style=flat-square" alt="简体中文 README"></a>
</p>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/latest"><img src="https://img.shields.io/github/v/release/Lyan-edit/CS2-nskinz?style=flat-square&label=release" alt="最新版本"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases"><img src="https://img.shields.io/github/downloads/Lyan-edit/CS2-nskinz/total?style=flat-square&label=下载量" alt="下载量"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz"><img src="https://img.shields.io/github/stars/Lyan-edit/CS2-nskinz?style=flat-square" alt="Stars"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/network/members"><img src="https://img.shields.io/github/forks/Lyan-edit/CS2-nskinz?style=flat-square" alt="Forks"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues"><img src="https://img.shields.io/github/issues/Lyan-edit/CS2-nskinz?style=flat-square" alt="Issues"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square" alt="C++20">
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/HLAE-离线%20Demo-6f42c1?style=flat-square" alt="HLAE 离线 Demo">
</p>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.5.4/Lyan_nskinz-v0.5.4-EN.dll"><img src="https://img.shields.io/badge/下载-英文%20DLL-2ea043?style=for-the-badge" alt="下载英文 DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.5.4/Lyan_nskinz-v0.5.4-zh-CN.dll"><img src="https://img.shields.io/badge/下载-中文%20DLL-0969da?style=for-the-badge" alt="下载中文 DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/tag/v0.5.4"><img src="https://img.shields.io/badge/正式版-v0.5.4-8250df?style=for-the-badge" alt="v0.5.4 发布说明"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues/new/choose"><img src="https://img.shields.io/badge/提交问题-f85149?style=for-the-badge" alt="提交问题"></a>
</p>

> **安全提示：** nSkinzCS2 只用于通过 HLAE、带 `-insecure` 启动的离线 Demo 回放。请勿加载到 VAC 保护服务器或任何联机服务器。超出此范围使用造成的账号风险由使用者自行承担。

## v0.5.4 更新

- 原 Demo 中已经带有网络涂装的枪械继续沿用 v0.4.5 已验证的实时路径；原皮／默认枪械先在生成的临时 Demo 中补齐经济字段，再继续使用同一套实时外观逻辑。
- 已有网络涂装的手套继续走带保护的实时路径；T/CT 默认手套先在临时 Demo 中重建定义、涂装、种子和磨损字段，再进行第二次回放。
- 目标刀动画骨骼不再要求存在于源 Demo。重写器会从已安装 CS2 的松散文件或 `pak01_dir.vpk` 验证目标骨骼，再插入标准 `AnimAssetData` 加载记录。
- 刀实体在 Demo 重载和代际变化时保留模型/动画监听器，并分离武器位置与 HUD 消费者，针对刀模落到手枪位置、手枪检视动作及原皮匕首动作残留进行修正。
- 原皮 AK-47 现在会解析经过验证的 CS2 2000919 材质生成入口，并且每个实体代际只执行一次引擎原生材质初始化；生成后的材质状态使用稳定缓存，不再逐帧刷新。
- 枪械、刀和手套均支持名称标签。自定义名称为空时使用 `nSkinz CS2`，填写后则序列化为指定名称。
- 创意工坊地图模型替换正式纳入发布版：自动识别已安装订阅，第一次加载 Demo 时配置并启用，第二次加载同一 Demo 时替换地图；同时安全抑制可确认属于原地图的生成模型。已烘焙进编译地图且没有安全一一对应关系的几何体仍可能残留。
- 创意工坊雷达现在会自动识别每张纹理的中性暗底／网格灰阶，清理 Ancient Night 的黑底网格；Inferno Water 恢复的边缘与窄路像素改为继承相邻道路颜色，不再出现不透明黑色残渣。
- 文件名、控制面板标题和启动日志均标记 `v0.5.4`。本次发布使用 CS2 Steam Build `25588766`、Client/Server `2000919`、Patch `1.41.8.6`、Source revision `11049427`；HLAE `2.192.6`。

地图模型／雷达、刀模、动作与默认载体仍需针对具体 Demo／地图组合进行画面验收。完整回归和隔离加载门禁已通过，但自动测试不能替代这一步。详见[中英双语 v0.5.4 发布说明](docs/releases/v0.5.4.md)。

## v0.4.5 更新（历史稳定版）

- 修复当前 CS2 中 Schema 能提供 72 字节属性布局、却没有向量修改回调时，原皮枪械无法创建涂装、种子和磨损节点的问题。新路径只在字节级验证通过后调用 CS2 自身的属性追加函数。
- 删除朋友日志中出现的两次完整实体扫描；通过已验证的 CS2 分块布局直接按索引读取，并在探测实体上界时同步填充缓存。
- 重复点击“更新”时复用已由引擎构造、随后安全清零的尾部节点，不再每次扩张向量。
- 当前独立刀模链已随源码和运行时进入本版，但明确标记为**实验功能**：刀模、切刀和检视已有进展，首次重建动作、队友发刀/拾取刀及所有 HUD 交接尚未完成验收。
- 编译输出及双语 ZIP 内文件命名为 `Lyan_nskinz v0.4.5 EN.dll` 和 `Lyan_nskinz v0.4.5 zh-CN.dll`，两者继续使用静态 `/MT` 与内嵌面板着色器。GitHub 会规范化带空格的附件名，因此单独下载的 DLL 使用稳定的连字符别名。

用户提供的 v0.4.4 日志显示枪械请求结束于 `changed_slots=0, verified=0`，同时外观轮询平均约 31 毫秒；v0.4.5 分别修复这两个原因。完整原生回归、当前客户端二进制只读签名检查和隔离加载检查已通过；朋友该 Demo 的画面验收仍需用户复测。详见[中英双语 v0.4.5 发布说明](docs/releases/v0.4.5.md)。

<details>
<summary>保留的 v0.4.4 修复</summary>

- 修复原皮枪械选择兼容涂装后仍显示无涂装的问题，包括原皮 AWP → 嘣。
- 通过引擎补齐缺失的涂装、种子和磨损属性，并为材质容器为空的物品执行首次生成；这是普通枪械共用修复，不是 AWP 特判。
- 重复更新复用已有属性；撤销前校验对象代际；初始化失败后停止当前对象/外观的自动重试，直到手动更新或对象变化。
- 保留枪型兼容筛选、新旧网格与 HUD 修复、完整目录、独立面板输入法/搜索和单 DLL 加载方案。
- 原样发布已测试的中英文 DLL，不混入暂停中的独立刀模续作。

用户已确认原皮 AWP → 嘣的 Demo 实测成功。**46 项原生回归**、**9 个定向检查**及隔离加载/渲染检查通过。目录覆盖 **34 枪型 / 1,449 个合法组合**，不代表逐个涂装都完成了画面实测。详见[中英双语 v0.4.4 发布说明](docs/releases/v0.4.4.md)。

</details>

<details>
<summary>保留的 v0.4.3 修复</summary>

- 按本机涂装元数据选择新旧枪械网格，修复 M4A4 从破浪狂飙换成皇帝仍显示原皮的问题。
- 修复网络实体缓存之外的第一人称枪身漏查；当前活动枪身应存在但尚未解析时，在任何外观写入之前保持等待状态。
- 枪械涂装列表按精确枪型兼容关系筛选；运行时和导入配置同样校验。保留完整目录，不兼容选项明确显示“不可用”，不会被偷偷改成原皮。
- 枪械刷新缓存增加实体身份与创建代际校验；对象未变、收枪或修复被回放覆盖的字段时不重复刷新。
- 保留独立面板输入法/搜索修复、静态 MSVC `/MT` 与内嵌着色器。独立刀模链保持只读，尚未完成可用的模型替换。

用户已确认本地 Demo 的皇帝修复成功。**45 项原生回归**、**8 个定向检查**、双语构建及隔离加载/渲染检查通过。目录覆盖 **34 枪型 / 1,449 个合法组合**，不代表逐个涂装都完成了画面实测。详见[中英双语 v0.4.3 发布说明](docs/releases/v0.4.3.md)。

</details>

<details>
<summary>保留的 v0.4.1 修复</summary>

- 修复使用输入法、在皮肤搜索栏输入文字导致 CS2 闪退的问题，包括英文输入触发的崩溃。完整控制面板现运行于 `cs2.exe` 之外，通过系统自带的 `rundll32.exe` 承载，无需额外下载程序。
- 独立面板保留本机游戏的完整皮肤目录及中文名称，不以缩减涂装选项规避输入问题。
- 修复枪械动态涂装属性同步和刀材质重复刷新。
- “仅饰面”隐藏并忽略刀模型选择，只有“饰面＋模型”才显示模型选项。
- 移除网格更新中的阻塞等待/重试、运行时反复复制完整目录，以及帧回调内同步写日志的开销。

v0.4.1 发布时通过 **42 项原生回归测试**。网格函数 240 次更新的基准从 3714 毫秒降至约 0.1 毫秒；这**不是游戏帧率实测，也不代表所有 Demo 卡顿都已解决**。验证范围和已知限制见[中英双语 v0.4.1 发布说明](docs/releases/v0.4.1.md)。

</details>

## nSkinzCS2 是什么？

nSkinzCS2 是一个 Windows x64 DLL，用于在观看 Counter-Strike 2 Demo 时修改当前观战玩家的外观。它通过 HLAE 的 `mirv_loadlibrary` 加载，读取本机 CS2 目录中的物品目录，并通过 Win32 + D3D11 + Dear ImGui 控制面板，按玩家 XUID 安全地应用外观覆盖。

![nSkinzCS2 控制面板](./docs/assets/control-panel.png)

## 功能矩阵

| 模块 | v0.5.4 支持情况 | 说明 |
| --- | --- | --- |
| 枪械 | 按枪型筛选的涂装、种子、磨损、品质、名称标签和贴纸字段 | 原 Demo 已带涂装时走原实时路径；原皮／默认枪械先由临时 Demo 补齐涂装身份。运行时及导入配置仍会校验兼容性。 |
| 刀 | 饰面、种子和磨损；实验性饰面＋模型替换 | 仅饰面隐藏模型选项；模型/切刀/检视替换已接入，但首次重建、拾取刀和 HUD 交接仍属实验功能。 |
| 手套 | T/CT 默认手套、手套模型和涂装 | 已带涂装的源手套走带保护的实时路径；默认定义 `5028`、`5029` 在临时 Demo 中重建后再应用其余实时字段。 |
| 临时换肤 Demo | 一键生成并加载包含完整刀记录及缺失的默认枪械／手套经济字段的第二个 `.dem` | 由 DLL 内嵌的开源 nSkinz 重写器独立实现；绝不覆盖原 Demo，检测到已有涂装的枪械／手套时原样放行。 |
| 创意工坊地图 | 自动识别已安装订阅；在 Demo 第二次加载时替换地图并抑制可安全识别的原地图生成模型 | 已纳入本测试版；不改写 `.dem` 或创意工坊文件。已烘焙几何体在无安全替代时可能残留。 |
| 挂件 | 修改或清除已有实时模块 | 对没有挂件模块的物品创建模块仍属实验功能。 |
| 原皮 / 无涂装 | 独立的 `原皮（无涂装）` 选项 | 与 `-（不修改）` 不同；部分 Demo 刀的第一人称旧材质可能残留。 |
| 语言 | 英文和简体中文 DLL | 中文版从本机 `csgo_schinese.txt` 读取名称。 |

## 快速开始

v0.5.4 已内嵌预编译面板着色器、独立 Demo 重写器并保留 `/MT`。除 Windows 10/11 x64、CS2 和 HLAE 外不需要其它运行库。`LoadLibrary failed` 排查见[单 DLL 加载说明](docs/portable-loading.md)。

1. 使用 HLAE、带 `-insecure` 启动 CS2，然后打开离线 Demo。
2. 下载一个 Release DLL。发布版使用 MSVC 静态运行库（`/MT`），不需要另装 Visual C++ Redistributable、Python、SDK 或 Visual Studio。
3. 在 HLAE 中加载 DLL：

```text
mirv_loadlibrary "D:\Lyan_nskinz-v0.5.4-zh-CN.dll"
```

英文版：

```text
mirv_loadlibrary "D:\Lyan_nskinz-v0.5.4-EN.dll"
```

上述示例要求 DLL 位于 D 盘根目录，请改成实际完整路径。ZIP 内 DLL 文件名包含空格，加载时必须使用一对英文半角双引号；不要叠加两对引号或使用中文弯引号。

按 `Insert` 显示面板，选择观战玩家 XUID 和外观配置，点击“更新”（`Update`）。请把 DLL 放在可写文件夹，独立面板与运行时需要在此生成和共享目录/配置文件。运行日志为 `nSkinzCS2.log`，面板日志为 `nSkinzCS2.ui.log`。

升级时请重启 HLAE/CS2，只加载**一个语言版本**的 DLL；先备份旧配置，不要在旧 DLL 已加载时叠加注入新版。[Windows x64 发布包](https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.5.4/Lyan_nskinz-v0.5.4-CS2-2000919-windows-x64.zip)含中英文 DLL、游戏版本、许可证／声明与加载说明。

## 创意工坊地图与模型替换

开始前请先订阅并完整下载目标 CS2 创意工坊地图。新增的“地图更换”页会读取 Steam 本地创意工坊清单，只识别顶层 `maps/<地图名>.vpk`，不会把地图内部 prefab 误列为独立地图。

1. 通过 HLAE 第一次加载原 Demo，并加载 nSkinzCS2。
2. 打开“地图更换”，点击“重新识别已订阅地图”，选择目标地图，勾选“启用 Demo 地图替换”，再点击“使用选中的创意工坊地图”。
3. “原始 Demo 地图”可留空自动捕获；在第一次加载仍处于活动状态时点击主界面的“更新”。
4. 确认日志出现 `Map remap ready` 和 `Map remap armed`。
5. 对同一个 Demo 再执行一次 `playdemo`；地图会在第二次解析／加载过程中替换。

只有当前 CS2 版本的 protobuf、KeyValues、manifest 与资源回退入口全部验证成功时才会启用；任一点缺失或不唯一都会整体拒绝安装，不留下半套 hook。该实现已通过编译、完整原生回归和本机创意工坊识别检查，但不同地图仍需在游戏画面中逐个验收。

逐 Hook 对照和有意保留的框架差异见 [MulNX 地图更换对照审计](docs/map-remap-mulnx-audit.md)。

## 临时饰品换肤 Demo

当前源码可生成第二个 Demo，让刀的定义、子类／模型和动画数据，以及原皮枪械／默认手套缺失的经济字段，在 CS2 解析新回放前就已存在。目标刀的动画骨骼不会从源 Demo 复制：工具会在当前 CS2 安装目录的 `game/csgo` 松散文件或 `pak01_dir.vpk` 中确认目标 `.vnmskel_c`，再写入让 Source 2 加载该已安装资源的标准 `AnimAssetData` 记录。归属扫描会对每一把枪和每一副手套独立判断：源物品已有网络涂装值时保持不动并交给原实时逻辑；源物品没有涂装时才写入临时 Demo。手套使用“仅饰面”模式时，会保留从源 Demo 解析出的手套定义，不会强制换成默认模型。

1. 第一次加载原 Demo，按观战玩家 XUID 配置枪械、刀和手套，然后点击“更新”，让运行时捕获当前 Demo 路径。
2. 打开“临时换肤 Demo”，点击“一键生成并加载临时 Demo”。DLL 内嵌的 nSkinz 工具先扫描归属和源涂装状态，再把完整刀事务以及缺失的默认枪械／手套经济字段写入新 Demo。
3. 成功后 nSkinzCS2 会自动排队执行 `playdemo`；日志会分别报告实际重写的枪械、刀、手套，以及因已有涂装而保持原实时路径的枪械／手套数量。
4. 新文件保存在 `game/csgo/nskinz_temp/`，文件名前缀为 `nskinz_skin_`；原 Demo 保持不变。“再次加载已生成 Demo”只重播成品，不会重复改写。

播放 nSkinz 生成的 Demo 时，只停用实时刀修改，以保留 Demo 内已经完成的模型／动作事务。枪械和手套配置继续启用：已有涂装的源物品直接走原实时逻辑；默认物品从临时 Demo 已补齐的完整经济状态开始，再应用仅能由实时路径处理的其余字段。多个 XUID 会在归属扫描中分别解析。实现边界和失败行为见[开发说明](docs/development/temporary-demo-skin-rewrite-20260922.md)。

## 兼容基线

当前 `v0.5.4` 源码／本地构建对应以下已检查基线。已有涂装的枪械／手套沿用原实时路径，原皮／默认载体使用临时 Demo 引导；地图模型／雷达、刀模、动作和默认载体仍需画面验收。精确迁移内容与测试边界见 [2026-09-26 换肤兼容检查点](docs/development/cosmetics-compatibility-checkpoint-20260926.md)。

- Steam app build ID `25588766`
- CS2 Client/Server `2000919`、Patch `1.41.8.6`、Source revision `11049427`（2026-09-28 14:35:19）
- `client.dll` SHA-256：`5440ADEF04E91C282E8D9815F51F8255AD95740920C9E8660FA1DDBB3C07A1F5`
- `animationsystem.dll` SHA-256：`7DD8030CD53880F6364AA0C7B9FABCB3770D26A9349C2E1655DF3F062FE71539`
- HLAE `2.192.6`；AfxHookSource2 `0.41.6`，SHA-256：`0E1378B81B5B82974EA8DD5C3E701E924D63C452927B50D64C1580F746D01F8B`
- `Source2Client002`、`ResourceSystem013`、`VFileSystem017`

CS2 更新可能改变内存布局。DLL 会记录检测到的版本，并在签名缺失或匹配不唯一时拒绝不确定的写入。

<details>
<summary><strong>从源码构建</strong></summary>

要求：Windows 10/11 x64、Visual Studio 2022 + MSVC v143、Windows SDK 10、Direct3D 11 开发库，以及用于目录检查的本地 CS2 安装。

```text
打开 nSkinzCS2.sln
选择 Release | x64       # 英文版
选择 ReleaseZhCn | x64   # 简体中文版
生成解决方案
```

输出文件为 `build/Release/Lyan_nskinz v0.5.4 EN.dll`、`build/Release/Lyan_nskinz v0.5.4 zh-CN.dll` 和 `build/Release/nSkinzCS2.Tests.exe`。`run-build-and-tests.cmd` 会构建并运行原生回归测试，`run-tests.cmd` 只运行现有测试程序。

</details>

<details>
<summary><strong>仓库结构与架构</strong></summary>

| 路径 | 作用 |
| --- | --- |
| `src/` | 启动、配置、目录解析、运行时、外观辅助逻辑和 ImGui 界面 |
| `tools/` | DIA/RVA 与资源查询辅助程序 |
| `third_party/` | Dear ImGui、MinHook、nlohmann/json 源码 |
| `nSkinzCS2.sln` | DLL 与原生测试项目的 Visual Studio 解决方案 |
| `run-build-and-tests.cmd` | 构建和测试脚本 |

`AppState` 连接启动、UI 和运行时线程；`WeaponAppearance`、`KnifeAppearance`、`GloveAppearance` 将配置决策拆成可在游戏进程外测试的辅助模块；运行时层会先校验接口、签名和实体绑定。

</details>

## 已知限制与路线图

- 刀模替换及部分动画/HUD 交接仍属实验功能，尤其是实体重建后首次切刀、队友发刀或拾取刀。
- 创意工坊地图更换依赖当前 CS2 二进制签名及目标地图资源；游戏更新或订阅下载不完整会使功能停用或加载失败，画面效果仍需逐地图验收。
- 如果 Demo 中的刀已经拥有编译后的自定义材质，选择 `原皮（无涂装）` 后第一人称视角仍可能显示旧材质。正数刀涂装已验证；该旧材质清理问题不在 v0.4.5 的已修复声明内。
- 当前优先读取已安装的 CS2 游戏数据，尚未完全改为实时物品 Schema 遍历。

后续计划包括：完善原皮材质清理、扩大刀模/动画覆盖、支持空挂件模块创建，以及跟随 CS2 更新维护兼容性。

## 常见问题

**修复输入崩溃是否禁用了中文，或减少了皮肤？** 没有。完整面板与 CS2 的输入/文字服务路径隔离，并读取导出的本机游戏目录，仍支持中文输入法和英文搜索。如目录异常变少，请反馈两份日志及目录文件的生成状态。

**为什么单把枪的涂装选项变少了？** v0.4.5 只显示与该枪型兼容的涂装，完整本机目录仍保留。兼容数据缺失时拒绝应用；旧配置中的错误组合会标为不可用，不会悄悄变成原皮。

**Demo 仍然卡顿怎么办？** v0.4.1 移除了已确认的阻塞操作，但不保证所有 Demo 都无卡顿。请保留 CS2 运行现场，反馈日志里的 `Runtime performance` 行，以区分游戏回调、外观轮询、网格更新和材质生成的耗时。

**为什么 `-` 不会清除涂装？** `-（不修改）` 的含义就是保持当前值不变。需要明确去除涂装时请选择 `原皮（无涂装）`。

**修改后没有变化。** 请先加载 Demo，确认观战玩家 XUID 和基础物品正确，再点击 `Update`；同时查看 `nSkinzCS2.log` 中的绑定或兼容性错误。

**为什么刀上仍有旧材质？** 部分 Demo 物品带有编译后的第一人称材质，当前 v0.4.5 原皮路径无法保证全部清除，详见上面的已知限制。

**应该下载哪个 DLL？** 单独下载时，英文界面使用 `Lyan_nskinz-v0.5.4-EN.dll`，中文界面和中文目录名称使用 `Lyan_nskinz-v0.5.4-zh-CN.dll`；双语 ZIP 内保留带空格编译文件名。两者兼容同一套 CS2 基线。

**仓库能打开，但图片显示破图标怎么办？** 某些网络无法访问 GitHub 的 raw 图片域名。本 README 已使用 GitHub 推荐的相对图片路径；如果仍然加载失败，可临时用管理员身份打开记事本，编辑 `C:\Windows\System32\drivers\etc\hosts`，追加：

```text
199.232.68.133 raw.githubusercontent.com
199.232.68.133 githubusercontent.com
```

然后运行 `ipconfig /flushdns`，重启浏览器并刷新页面。网络恢复后可删除这两行；如果仓库本身在无痕窗口也返回 `404`，hosts 不能修复账号级公开可见性，需要联系 GitHub Support。

## 贡献与支持

提交 Pull Request 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请参考 [SECURITY.md](SECURITY.md)。反馈问题时请附上准确的 CS2 Build ID、HLAE 版本、DLL 语言、复现步骤和经过脱敏的日志片段。

<a name="support"></a>
## ❤️ 支持项目

如果 nSkinzCS2 对你的离线 Demo 工作流有帮助，欢迎自愿支持。下方是作者提供的无 ID 收款码；赞助完全自愿，不代表提供技术支持合同或功能保证。

| **微信支付** | **支付宝** |
| :---: | :---: |
| ![微信支付收款码](./docs/assets/sponsor/wechat-pay.png) | ![支付宝收款码](./docs/assets/sponsor/alipay.png) |

## 致谢与授权说明

nSkinzCS2 v0.5.4 以 [`AGPL-3.0-only`](LICENSE) 发布。地图更换实现移植并核对自 [MulNX_CS2](https://github.com/Co1Swet/MulNX_CS2) 提交 `361008b208326d9e7916ff2cd58dd57a0492c400`，并从 `f2466e18e26a8fc3daf4ac7945bc45220ba95993` 同步当前 CS2 的 GameSession 兼容变更；本项目不使用 MulNX 文件改写器。发布附件同时提供 DLL 对应的完整源码。再分发或修改时必须保留 [`NOTICE.md`](NOTICE.md)、AGPL 许可证、源码提供义务和 `third_party/` 内各组件声明。

临时换肤 Demo 由 nSkinz 自有代码和随源码提供的开源 `source2-demo` 解析／写入器实现。其 MIT／Apache-2.0 声明和源码位于 `third_party/source2-demo`，并记录在 [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)。

<p align="center"><a href="https://star-history.com/#Lyan-edit/CS2-nskinz&Date"><img src="https://api.star-history.com/svg?repos=Lyan-edit/CS2-nskinz&type=Date" alt="Star History" width="760"></a></p>

<p align="center"><sub>为可复现的 CS2 离线 Demo 回放而构建。请保持离线并安全使用。</sub></p>
