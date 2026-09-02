# 刀模续作检查点：成对事务与更新时机探针

> v0.4.4 发布后的最新续作入口：[真实模型资源与图骨架引用](knife-model-checkpoint-20260831-resources.md)。以下保留暂停前历史，不要再加载旧开发 DLL 作为最新探针。

日期：2026-08-31。接续 [M9 / 骷髅只读实测](knife-model-checkpoint-20260831-live.md)。

## 当前暂停点（2026-08-31，转查 v0.4.3 AWP 饰面问题）

用户要求保存刀模进度，先排查朋友在 Demo `131331331` 中原皮 AWP 改为“嘣”无效；朋友明确使用正式 v0.4.3。此问题不应归因于尚未发布的刀模代码，也不要为排查 AWP 继续加载刀模探针。

- 用户已于 12:04:27 加载 `build/knife-transaction-dev/zh-CN/nSkinzCS2.zh-CN.dll`，12:05:16 从面板更新 NiKo（76561198041683378）“饰面+模型”，目标 M9（508），并播放 Demo。之前文中的“尚未加载”仅指加载前采样。
- DLL 内部只读绑定仍全零，未尝试安装 timing Hook；PDB 对应的只读运行状态及日志保存在 `build/knife-transaction-dev/runtime-state.json`、`live-loaded.json`、`zh-CN/nSkinzCS2.log`。外部读取却发现有效骷髅刀、原生参数图与 HUD 副骨架，说明此前只读探测受前置观察者门槛阻断，不能解释为没有动画图。
- 当前源码已把候选实体/原生图的只读发现与旧观察者门槛分离，并增加 ASCII 诊断字段。真正换模的 `ownership_verified` 仍同时要求旧观察者与原生绑定验证；`native_pair_adapter_verified=false`、`safe_point=false`，Windows 原生写入端口仍未接通。
- 新一轮仅编译了中文探针 `build/knife-transaction-probe2/zh-CN/nSkinzCS2.zh-CN.dll`，构建成功；**未加载实测，未对这一轮改动重新完成中英构建/回归/便携性测试**。不要把上一轮 47 项测试结果套用于新探针。
- `tools/inspect_nskinz_state.py` 是基于本地匹配 PDB、DbgHelp 和 ReadProcessMemory 的只读状态检查器，不注入、不暂停目标进程。图字段及 Hook attempted/counters 的结果来自该工具，不是换模成功证据。
- 正式 v0.4.3 DLL、标签、Release 未改动。恢复刀模工作时从本节继续，不重复假设用户尚未加载旧探针，也不热覆盖已加载的 DLL。

**本阶段不是可用换刀模型版。** 独立成对事务已经实现并接受故障注入测试；Windows 原生 `KnifePairAccess` 端口尚未接通。开发 DLL 增加的 Hook 仅观察更新时机，模型和动画写入仍关闭。不得把模拟测试或 Hook 计数标成游戏内换模、检视、穿模测试成功。

## 新增实现

- `src/Runtime/KnifePair.hpp/.cpp`：具体 `KnifeModelAdapter` 实现，调用窄接口 `KnifePairAccess`。状态仅包含刀身模型、手臂模型的只读身份、刀型原生 symbol、副骨架与附件验证结果，不包含涂装、种子、磨损、贴纸、挂件、手套或整个物品快照。
- 提交顺序：世界刀身 SetModel → 第一人称刀身 SetModel → **显式原生副骨架重收集** → 对已重新验证的 ID 参数作条件更新。每一步前后检查身份和状态，失败不在每帧重试。
- 恢复只使用原始刀型对应的静态资源路径重新调用原生 SetModel，不把旧 `CStrongHandle` 指针写回。实体 serial、创建 tick、scene epoch、图身份改变时拒绝恢复；第三方写入非本事务的刀型或动画值时不覆盖。
- `AfterNativeUpdate` 用于未来的原生更新后维护：仅条件更新 `weapon_type`，绝不在该方法里重设模型或重收集资源。目标请求、模型绑定、手臂、副骨架、附件或线程时机失效时锁停维护，等事务所有者明确退场。
- `src/Runtime/KnifeUpdateProbe.hpp/.cpp`：观察 `0x8E5390` 的独立诊断 Hook，两个参数原样传给 trampoline，返回后采样调用者、线程、参数图与源刀型。无模型写入、无动画写入、无符号创建、无资源加载，也不改变共享 VData。
- `Runtime.cpp`：仅 DLL 面板选择“饰面+模型”、实际持刀归属/图/副骨架验证通过、HLAE 加载、`-insecure`、离线 Demo、当前线程等于 FrameStageNotify 线程时安装并发布探针目标。仅饰面不安装，非目标武器回调直接经过原函数。
- 诊断日志由原有低频轮询输出，最多每 5 秒一次；原生回调本身不记录日志、不扫描实体、不分配字符串、不加生命周期锁。

安装与停用互斥；原始 trampoline 在启用前发布。失败不逐帧重试。清理时禁用 Hook，但不立即释放仍可能在被调用的 trampoline；每进程保留一个有界分配到退出，不按玩家或 seek 累积。本开发模块不提供热卸载能力，替换开发 DLL 应重新启动游戏。

## 本轮新确认的原生路径

基线仍为 CS2 Client/Server 2000899、Patch 1.41.7.8、Steam Build 25000182，HLAE 2.191.1。以下为当前本地二进制的静态证据，不是已调用验证：

| 原生位置 | 已确认用途 |
| --- | --- |
| `0x934CA0(entity, path)` | 字符串 SetModel 包装器，查询模型 binding 后进入 `0x934CE0` |
| `0x934CE0(entity, binding)` | 通过 BodyComponent 更新模型，并调用实体虚函数 `+0x780` 模型变更通知 |
| `0x1E1580 → 0xA80090 → 0xA7FF20` | 模型状态转换及强引用赋值；`A7FF20` 内部对新 binding `+0x20` 加引用、旧 binding 减引用，归零调用引擎资源所有者释放路径 |
| HUD `+0x780 → 0x104B2D0` | HUD 模型变更后的基类处理和附件重建；不能用写名称/句柄替代该通知 |
| `0x901370` 内 `0x90143D..0x90144B` | 当 controller `+0x54` 非零时调用 `0x9040C0`，然后清除该标志 |
| `0x37C700` 内 `0x37C7B1` | 特定 AG2 更新条件下也会重收集副骨架；并非任意暂停状态都会执行 |
| `0x104E414` / `0x104E438 → 0x8E5390` | 参数为 graph controller 和活跃世界武器；原生更新从源武器 VData 重写 `weapon_type` |
| `0x104E4CF → 0x37ADA0` | 上述更新之后的真实图更新调用；中间 `+0xD0` 仍只是算法查询 |

结论：使用完整原生 SetModel 包装器时，独立刀模事务不需要复制旧手套实验路径的手动引用计数/裸句柄写入。只保存原始静态模型路径，资源持有交给引擎；恢复时也重新走原生路径。

仍未证明 HUD SetModel 的通知在所有情况下都会把父 arms `+0x54` 置位。**不可依靠同槽重新登记的早退路径，也不可宣称自动重收集已经闭合。** 新事务要求显式调用已定位的重收集器，并随后验证模型所带骨架确实进入当前 arms 图。

## 测试边界

新增 `src/Tests/KnifePairTests.cpp`：6 组测试，包含 22×22=484 组模拟组合（含 22 组同刀型不操作）、每个提交阶段写前/写后失败、生命周期与竞争写入、参数维护门槛、10 万次回调无额外 SetModel/重收集。

`KnifeGraphTests.cpp` 额外检查真实 PE 的 updater 入口、两个调用者、后续图更新共 4 个精确指令窗，逐字节破坏和各窗截断必须拒绝。原有图与副骨架 7 个布局窗仍保留。

本轮后台只读发现 `build/knife-transaction-dev/live-preload.json`：当前进程 PID 240600 仍为激活骷髅刀视角，发现过程无错误；模块列表含 HLAE，但尚无 nSkinz DLL。因此**此时没有新 Hook 运行计数**。不能把外部进程读取称为已经加载开发 DLL 实测。

最终构建、回归与 DLL 哈希记录在 `build/knife-transaction-dev/VALIDATION.md`。正式 v0.4.3 发布目录、标签、Release 及 GitHub 未改动。

## 下一次进入游戏的步骤

1. 在 HLAE `-insecure` 离线 Demo 中加载 `build/knife-transaction-dev/zh-CN/nSkinzCS2.zh-CN.dll`，不要重复叠加其它版本 DLL；已有旧 DLL 的进程先重启。
2. 用 nSkinz 面板选择当前玩家，启用刀的“饰面+模型”，目标选一个与原刀不同的刀型，然后 Update。此包仍不会显示目标刀模。
3. 用户自行播放 Demo，观察持刀、收刀再切刀/检视。助手后台读取该目录的 `nSkinzCS2.log`，核对 `Independent knife update timing` 中两个调用点、游戏线程、源值匹配和拒绝计数。
4. 不根据单个回调或只暂停持刀时的样本自动开放原生写入；验证播放/切换后的实际回调时序与资源生命周期后，才实现并接入 Windows `KnifePairAccess`。

原生端口续作必须完成：

- `Read` 里的 world/hud definition 按 **实际模型路径** 映射，不按 Econ 物品定义读取；物品定义仍由饰面链处理，不用它冒充目标模型验证。
- 原生 SetModel 返回后，读取真实 model binding 与 `model +0x430/+0x438` 的 `m_vecNmSkeletonRefs`；检查目标骨架进入 `graph +0x338` 的副骨架引用列表，再放行动画分支。`0x16559E0(model, out, index)` 是三参数取引用函数，有输出参数，不能误声明为一/二参数函数。
- 单独验证模型变更后 HUD 附件与 arms 原模型保持不变，不向 arms 塞入刀身 skeleton。
- `PollIndependentKnifeModel` 目前的源刀型只读门槛，接入维护后要区分“原生更新刚写回的源 symbol”和“本事务已提交的目标 symbol”。只允许同一已捕获 binding 的预期两值，不能删除图归属/type 检查。
- 切刀收刀、seek、换玩家、禁用和退出时的退休路径需要真实回调安全点。图被引擎替换时放弃旧地址，而不是强行回放快照。
- 原生端口、事务与面板接通后，才开始实际刀模、切刀/攻击/检视/特殊检视、穿模和帧时间测试，并回归刀饰面、枪皮和手套。
