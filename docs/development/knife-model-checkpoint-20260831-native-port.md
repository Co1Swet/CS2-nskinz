# 刀模续作：原生端口实现，生命周期接线未完成

日期：2026-08-31。接续 [资源检查点](knife-model-checkpoint-20260831-resources.md)。正式 v0.4.4 源码包、DLL、标签和 Release 未改动；本轮没有推送或发布。

## 用户这次看到的“回退”是什么

用户在 `knife-resources-dev` 中文 DLL 面板选择 NiKo（76561198041683378），模式 `model_and_finish`，目标 M9（508），涂装 568。保存后的 JSON 保留这些值，UI 日志记录了 Update/Save/Load，没有配置被重置的证据。

17:06:31–17:06:41 的实际日志显示：

- 源刀型 525（骷髅），真实 world/HUD 模型均为 525。
- `native_binding_verified=1`、`source_pair_ready=1`、`secondary_verified=1`。
- 世界刀身与 HUD 的骨架匹配，HUD 骨架已进入同一 arms 图。
- 更新回调累计 `matched=1920, call1=0, call2=1920, game_thread=1920, source_verified=1920, rejected=0`。
- `legacy_observer_verified=0`、`owner_verified=0`；旧观察者门槛仍未闭合，不能直接删除。
- `model_writes=0, animation_writes=0`。这个版本本来就没有接通模型写入，因此不是换模成功后又回滚，而是只有独立涂装链在工作。

本轮曾只读检查 PID 245596，确认加载的是 `build/knife-resources-dev/zh-CN/nSkinzCS2.zh-CN.dll`，HLAE 路径为 2.191.1，当前刀型仍为骷髅。报告保存在 `build/knife-resources-dev/live-user-report.json`。随后该 CS2 进程退出；新原生端口检查器报 `OpenProcess read-only failed`，此后进程枚举也无 CS2。**不能把这次失败当成新端口验证通过，也不能仅凭此断定退出原因。** 助手没有启动/停止游戏、注入、控制窗口或输入。

## 已实现的原生端口

新增 `src/Runtime/KnifeNative.hpp/.cpp`，实现 `KnifeNativePairAccess`，由原有 `KnifePairAdapter` 驱动，而不是复制另一套涂装替换链。

- `Read` 使用真实 `CModelState::m_ModelName` 识别模型，不使用 Econ definition 或显示名称。只允许本事务的原始/目标刀型；名称读取固定 256 字节上限，无堆分配。
- 读取真实加载模型、骨架引用、ID 参数节点、HUD 副骨架；保留手臂模型身份。源/目标 symbol 仅在当前上下文允许，不放宽通用只读图解析器。
- `SetBladeModel` 只接受世界刀身或 HUD 刀身、规范源/目标资源。调用当前引擎的字符串 SetModel 包装器，让引擎维护强引用并发送模型变更通知，不写裸句柄，不改共享 VData。
- `Recollect` 调用显式副骨架重收集器；随后检查目标引用是否进入当前图，不能只看函数返回。
- `CompareAnimationType` 重新核对 ID 节点类型、定义索引、图所有权及当前值，再对 symbol 条件交换；交换后验证仍是同一节点。无固定序列编号、无新建 symbol。
- Windows 调用原语有独立 SEH 边界；模拟测试覆盖原语，避免把“假端口通过”当成原生端口通过。**SEH 不是安全时机或生命周期证明。**

运行时新增 `IndependentKnifeNativeHost`：验证四实体完整 handle/serial/创建 tick、epoch、当前持刀、pawn/arms/HUD owner、scene/model 字段位置及 body controller。`ReadIndependentKnifeNativePort` 在低频轮询中调用真实端口的只读部分。

`IndependentKnifeNativeHost::AtSafePoint` **仍固定返回 false**。`PollIndependentKnifeModel` 仍使用只读控制器适配器，更新 Hook 也仍只记录计数。因此本轮虽然实现了可测试的 Windows 原生端口，**没有启用持续换模，也不是可以给用户验证刀模效果的版本**。

新日志明确输出 `write_gate=retirement_pending, model_attempts=0, rollback_attempts=0`。面板在独立开发链状态下显示“模型写入尚未启用；当前只应用涂装；没有执行换模回滚”，中英文均已加入；不会把检查通过标成换模成功。

## 新增原生 ABI 证据

当前基线仍是 CS2 Client/Server 2000899、Patch 1.41.7.8、Steam Build 25000182，Source revision 10948930；本机 HLAE 2.191.1。

五个精确指令窗已加入生产验证器，并与本机磁盘 client.dll 逐字节对照：

| RVA | 检查内容 |
| --- | --- |
| `0x934CA0`，49 字节 | 字符串 SetModel 的资源查询、参数传递及调用 `934CE0` |
| `0x934E48`，23 字节 | 模型处理后执行实体 `+0x780` 通知 |
| `0x9040C0`，31 字节 | 副骨架重收集入口与 controller `+0x450` 图检查 |
| `0x9041E0`，15 字节 | `(model, out, index)` 三参数骨架 getter 调用 |
| `0x90431B`，16 字节 | 对当前图提交收集后的引用列表 |

每一窗逐字节变更/截断测试均拒绝。已有资源布局与图布局验证继续保留。

本轮再次静态确认：`0x104E310(arms, bool)` 包含两个原生 weapon_type 更新调用以及后续 `0x104E4CF -> 0x37ADA0` 图更新。它在某些无有效武器的分支也会走向图更新，因此可以作为后续退休时机调查的候选；**当前未 Hook 此入口，也未验证它在收刀/换人/无武器时的现场调用和线程行为，不能当成现成安全点。**

## 验证与边界

- EN / ZH-CN DLL、测试程序、外部只读检查器编译成功，产物位于 `build/knife-native-port-dev/`。
- 刀模 3 个入口通过（模型 10 组、图/原生端口 11 组、成对事务 6 组）；9 个枪皮入口通过；完整 48 项回归通过（75.812 秒）。
- 新原生端口 fixture：实际模型路径/资源读取；无安全点零写入；提交顺序；128 次原生源参数复位后维护不重载模型；四个写入阶段、写前/写后共 8 类失败恢复；11 类绑定/路径/symbol 异常；指令窗损坏；竞争值不覆盖。
- 既有 484 种刀型组合与 10 万次维护是模拟测试；新增测试也没有调用真实游戏原生函数。不能宣称攻击、检视、穿模或 FPS 实测通过。
- 两语言六项隔离加载、无窗口 WARP/DXBC/ImGui 像素读回和 3 次重建通过；PE 依赖与中文文本检查通过，不是纯净虚拟机测试。
- 新外部工具使用同一原生端口的只读 `Read`，显式禁止三种写原语，即使被误调用也不会把远程地址当本地函数。**由于进程已退出，此次新工具未完成现场验证。**

详细哈希与验证路径见 `build/knife-native-port-dev/VALIDATION.md`。新的独立源码快照位于 `build/checkpoints/knife-native-port-20260831/`；不覆盖资源阶段快照。

## 下次从这里继续，不要再重复“只读探针应能换模”测试

1. 处理事务退休：收刀、切枪、换玩家或禁用之后，旧刀可能不再收到 `8E5390` 回调。需要验证仍可恢复原模型的游戏线程时机；不能简单 Clear 后遗留目标世界刀，也不能向已换所有者的 HUD 写入。
2. 分开实体存活与图存活。图重建、HUD 被销毁、pawn 仍存活时，当前通用 `Abandon` 不能证明世界模型已恢复。seek 必须丢弃旧 epoch，绝不重放旧指针。
3. 查旧 `observer_resolution` 的失配原因；原生 owner/secondary/图和实时更新绑定提供了独立证据，但尚未建立完整的观战授权替代路径。
4. 用已捕获事务精确限定源/目标 symbol 与模型，修改低频 Poll 的源刀型只读门槛，否则提交目标后它会把目标误判成来源失配并 Clear 探针。
5. 新端口的 `Read` 包含完整路径、资源与绑定复核。未来不能不经性能测量就把整套复核放进每帧维护；保留结构变动时完整校验，稳定期只做有界必要验证，不每帧 SetModel/recollect。
6. 完成这些接线和故障测试后再开放原生事务。随后用户通过 nSkinz 面板测试真实切刀/攻击/检视、穿模、帧时间及枪皮/刀饰面/手套独立性。本轮不要求用户重复加载另一个不换模的探针。
