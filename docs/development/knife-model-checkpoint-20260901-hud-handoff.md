# 刀模暂停点：修复世界刀通知导致的 HUD 延迟销毁（2026-09-01）

接续 `knife-model-checkpoint-20260831-lifecycle.md`。用户实测上一版后报告“换模失败”。本轮保存现场、进行只读检查并修改独立刀模事务；未控制桌面、未注入或写入当前 CS2 进程、未发布 Release。枪皮、刀饰面、手套实现保持不变。

## 现场与根因

- 失败进程 PID 65772 实际加载 `build/knife-lifecycle-dev/zh-CN/nSkinzCS2.zh-CN.dll`，不是公开 v0.4.4。配置为 NiKo / XUID `76561198041683378`，Skeleton 525 → M9 508，饰面＋模型，paint kit 572。
- 旧日志记录 `attempts=12, applied=12, discarded=12`，随后总尝试达到 13。世界刀、玩家、arms 和图基本不变，HUD 的完整 handle/identity 反复改变。即时提交内部校验通过，却无法保持目标模型；这不能算换模实测成功。
- 原生外层 Hook 已有真实计数：matched 195（call1=2 / call2=193）、source_verified 195、rejected 0、boundaries/nested_updates 1188。原有暂停点的“外层时序尚未实测”已被本次现场证据更新；新两阶段 DLL 则仍未实测。
- 已安装 client 的 `SetModel` 包装 `0x934CA0` → `0x934CE0`，在 `0x934E48` 调用模型变更通知虚表槽 `+0x780`。`C_Knife` 此槽实际指向 `0x7F6A70`：读取世界刀 `+0x1760` 的 HUD 完整 handle，解析后在 `0x7F6AD7` 尾调用 `0x9E2900`，进入实体删除队列。静态反汇编与当前进程只读 vtable 结果一致，报告 `deferred_hud_removal_verified=true`。
- 上一版在同一回调中先 SetWorld、紧接着 SetHud/recollect/type。被修改的 HUD 已因世界刀通知排队销毁，随后清理又恢复世界刀，形成回退循环。旧模拟器没有模拟这个延迟副作用，所以出现“回归通过、现场失败”。

原始日志、配置与只读报告保存在 `build/knife-hud-handoff-dev/evidence/`；包含用户测试信息，仅本地留档，不上传。

## 两阶段实现

1. `KnifePairAdapter::PrepareWorld`：捕获原始模型/参数，在验证过的原生回调仅更换世界刀，进入 `WaitingHudRebuild`。此时不写旧 HUD、不 recollect、不改动画类型。
2. `KnifeNativeHost::WorldBindingAlive`：等待期间单独证明 epoch、世界刀和 pawn 身份、当前持刀、SteamID 双向归属、源 Econ definition 和已验证的模型通知槽。旧 HUD 消失不再触发世界刀恢复。
3. 低频只读发现允许**同一已准备世界刀事务**出现世界刀 target / HUD source 的临时组合。不能以此放宽其它对象的验证；候选仍需完整实体/图归属、挂点及 HUD 副骨架登记。
4. `RebindPreparedHud`：只接受不同身份的新 HUD；旧 HUD 必须明确 Gone/Replaced，Live/Unreadable 不可接受。保持同一世界刀、玩家、epoch、arms/控制器、世界/arms 资源字段、源目标符号、原生证据；重新验证候选及世界刀目标资源身份。允许原生重建新的 graph instance，不重放旧参数节点地址。
5. `CompletePreparedWorld`：接入新 HUD，必要时只更换其模型，再收集副骨架、条件更新 `weapon_type`，核验后才记 `applied`/`hud_handoffs`。如果 HUD/参数已原生继承目标，不重复加载或无条件写入；继承的 HUD 仍纳入源模型恢复账本。
6. 3 秒没有可验证后继 HUD，则恢复世界刀并锁存同世界刀/epoch/目标的失败；HUD 身份继续变化也不重启每帧 SetWorld 循环。禁用、改目标或收刀时同样独立清理。既有跨 seek 地址丢弃和恢复失败阻止新事务机制保留。
7. 稳定阶段仍不 SetModel/recollect，动画节点每次重新验证；500 ms 完整校验、250 ms 候选发现。日志新增 `world_prepared`、`hud_handoffs`，把 HUD 接续成本排除出 `steady_max_us`。计数/耗时仅为内部诊断，不是画面或 FPS 验收。

没有写源刀的 Econ definition、共享 VData、arms 模型或旧 sequence；不接回旧的全局换模链。现有饰面应用链未改动。

## 产物与后台验证

开发版目录：`build/knife-hud-handoff-dev/`，中英文 DLL 分别位于 `zh-CN/`、`en/`。旧 `knife-lifecycle-dev` 未覆盖。源码快照：`build/checkpoints/knife-hud-handoff-20260901/source-checkpoint.zip`，逐文件/产物 SHA256 与验证报告见同目录 `manifest.json`。

游戏安装基线重新读取确认：CS2 Client/Server **2000899**、Patch **1.41.7.8**、Steam Build **25000182**、Source revision **10948930**；HLAE **2.191.1**。此版仅针对此证据基线，不承诺后续游戏更新兼容。

最终后台验证已通过：

- EN / ZH / Tests / 只读 Probe 四个目标构建成功，静态 MSVC 运行库不变；存在原有编译警告，没有编译错误。
- 三个刀模入口通过（模型 10 组、图/端口/恢复/接续/生命周期 14 组、成对事务 6 组）；本机 PE 指令证据包含新增模型通知/删除路径的逐字节拒绝测试，通过。
- 9 个枪皮回归入口通过；完整 **48 项**通过，用时 **112.469 秒**。见 `build/knife-hud-handoff-dev/validation/report.json`。
- 中英文共六项隔离 DLL 加载通过，无窗口 WARP/DXBC/ImGui 像素读回及三次重建通过；见 `portable-load.json`。这不是朋友电脑或纯净虚拟机测试。
- PE 仅依赖 Windows 系统 DLL（d3d11/user32/shell32/version/kernel32/imm32）；中文 UI、原皮词条和游戏中文皮肤文本检查通过，没有新增可再发行运行库依赖。
- 新源码/开发文档 UTF-8、工程 XML 与快照内容在归档时校验；产物 SHA256 见 `VALIDATION.md` 和暂停点 manifest。旧 lifecycle DLL 的 SHA256 亦校验未变。

新增模拟场景包括：世界刀通知后旧 HUD 延迟销毁、旧 HUD 仍可读、新 HUD 使用源或目标模型、新 graph、旧 HUD 不可读/仍存活、错误归属/字段/符号、超时、新 HUD 不触发失败循环、等待中禁用或收刀、原生继承目标 HUD 的恢复。模拟器新增 HUD 的 handle 和地址独立于 arms，避免身份碰撞掩盖生产行为。

## 下一步现场验收

新 DLL **尚未在游戏中加载和验收**；不要再把即时 `Applied` 当成最终成功。

1. 由用户退出旧 CS2，用 HLAE `-insecure` 启动离线 Demo 新进程；不要向已加载旧 nSkinz 的进程重复注入。
2. 加载本目录中文 DLL，面板选择同一 NiKo / Skeleton → M9，“饰面＋模型”，点击更新并播放 Demo。观察 `world_prepared=1` 后 `hud_handoffs=1, applied=1`，并且不反复增长 attempts/discarded。
3. 实际检查刀身几何、拔刀、攻击、完整检视、握持/穿模、切枪后再切刀。若回退，保存两份日志及配置，重点看 source/target 模型定义、world_pending、HUD identity 和接续计数。
4. 同场景检查仅饰面、禁用、改目标、换玩家和 seek；再测同时改枪皮/手套/刀饰面。帧时间与外观都需要实际游戏验收。

加载指令：

```text
mirv_loadlibrary "E:\codex项目\CS2-nskinz-main\build\knife-hud-handoff-dev\zh-CN\nSkinzCS2.zh-CN.dll"
```
