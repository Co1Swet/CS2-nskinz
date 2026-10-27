# 独立刀模链开发记录

> 2026-10-27 修复进度：[临时 Demo 刀型与击杀信息同步](knife-killfeed-demo-rewrite-20261027.md)。已实现按攻击者和队伍同步刀杀事件，真实 Demo、混合饰品重建及 DLL 加载验证通过；修复纳入 v0.6.5，实际游戏图标画面仍待验收。以下内容保留历史开发记录。

> 当前续作状态（2026-09-07）：在 v0.4.5 开发链上修正主动刀的等待门槛、将原生回调收窄到目标实体，并接入 Pawn 原生手套可见模型链；同时针对旧日志里 8–13 ms 轮询和 202 ms 手套工厂峰值修正掉帧路径。最新开发 DLL、哈希、后台回归和待实测项目见 [刀模、手套可见链与掉帧修复检查点](knife-glove-performance-checkpoint-20260907.md)。此前首帧/M9 现场证据见 [2026-09-04 检查点](knife-model-checkpoint-20260904-first-frame-m9.md)；队友发刀/`.drop` 所有权保护见 [v0.4.5 合并检查点](knife-model-checkpoint-20260902-v045-merge-drop-ownership.md)。下文保留历史设计记录。

更新：2026-08-31。阶段：独立状态机 + 实体/动画图参数只读验证；不是已完成的换刀模版本。

最新续作：[成对事务与更新时机探针](knife-model-checkpoint-20260831-transaction.md)。已实现独立成对提交/回退与动画维护事务，新增原生更新后的诊断 Hook；实际原生换模端口未接通，尚未开放模型/动作写入。此前的图和副骨架验证见 [M9 / 骷髅刀只读实测检查点](knife-model-checkpoint-20260831-live.md)。

M4A4 新旧模型涂装修复已随 v0.4.3 发布，现按用户要求恢复刀模开发。历史见 [原暂停点](knife-model-checkpoint-20260831.md)；当前实现、节点数量/ID 类型证据和回归记录见 [动画图参数续作检查点](knife-model-checkpoint-20260831-graph.md)。

## 当前交付边界

- 已实现 `Runtime/KnifeModel.hpp/.cpp`，刀模请求只包含玩家 XUID 和目标刀型，不包含 paint kit、seed、wear、名称、枪皮或手套数据。
- 生产轮询已接入独立探针；通用饰面流水线只接收 `BuildKnifeFinishRequest`，不再启动旧资源路径替换、刀模重绑定和旧 sequence 动画写入。
- 独立事务支持完整实体身份、切换目标、切换玩家、收刀、Demo seek、失败回退、失败锁定、限频校验，以及成功后接纳新的动画图实例。
- **当前原生适配器是 `KnifeModelReadOnlyAdapter`，不会写模型或动画。** 事务执行与回退通过模拟适配器验证；不能把单元测试通过表述为 CS2 中换刀模成功。
- 枪、刀涂装、手套和挂件原有功能继续走各自路径；本轮没有进行游戏内回归，因此不承诺实际帧率、穿模或动作已经验证。
- 最新中英文开发 DLL 位于 `build/knife-pair-dev/en/` 和 `build/knife-pair-dev/zh-CN/`；`build/knife-graph-dev/`、`build/knife-model-dev/` 是历史阶段产物。它们是只读刀模探针开发版，不应代替正式 Release 向其他用户分发。
- 本次刀模续作没有覆盖 v0.4.3 正式 DLL，没有推送 GitHub、移动标签或修改 Release。

## 静态核对基线

| 项目 | 本机核对值 |
| --- | --- |
| CS2 ClientVersion / ServerVersion | 2000899 |
| PatchVersion | 1.41.7.8 |
| SourceRevision | 10948930 |
| Steam Build ID | 25000182 |
| 游戏构建时间 | 2026-08-28 13:05:38 |
| HLAE 基线 | 2.191.1；最新一轮在用户自行打开的离线 Demo 中后台只读验证 |
| client.dll SHA-256 | `B8E2C009763E8CEFB88D89A2BDCF452DB17501553D473DB6DA060DF8E6769EB4` |

这些是静态分析基线，不是新的实测兼容认证。公开 DLL 仍需 Windows、CS2 和 HLAE，不新增其它运行库要求。

## 原有实现路径的判定

| 路径 | 本轮修改前的使用情况 | 判定 |
| --- | --- | --- |
| `TryEnsureWatchedKnifeModelPathSwap` + 通用饰面重绑定 | 有生产调用 | 依赖全局资源查询/缓存证据，缺少完整的动作事务；已从生产刀涂装入口断开 |
| `TryApplyKnifeModelAndFinishPath` | 没有生产调用 | 旧实验支线，保留供分析，不能算正在工作的换模实现 |
| `TryProcessKnifeAnimationHook` | 仅旧支线调用 | Controller 字段曾被按实体偏移使用；不能接回生产写入 |
| `TryWriteKnifeSyntheticRenderProxyModelState` | 无调用 | 刀身/手臂归属和挂点未证明，不采用 |
| Resource override hook / GlobalResourceSwapper | 安装入口被关闭 | 不能把“存在代码”当作可用链路 |
| 新 `PollIndependentKnifeModel` | 已接入生产轮询 | 只读验证真实持有刀、HUD 刀身、手臂和图控制器；尚未开启原生写入 |

旧 remap 表只是 source/target/action 的组合集合，不是实际动作映射。新增 526（廓尔喀刀）到旧定义识别表，不代表旧 remap 已获验证。

## 模型与动作的证据

使用 [ValveResourceFormat / Source 2 Viewer](https://github.com/ValveResourceFormat/ValveResourceFormat) 20.0 CLI 解码本机资源，仅在 `build/temp/knife-model-audit/` 保留分析副本。CLI 下载 SHA-256 与上游 Release digest 一致。该工具不随 DLL 运行，也不分发 Valve 资源载荷。

1. `scripts/weapons.vdata_c` 的 22 个已发布刀型已逐一核对：模型路径、武器骨架路径和动画类全部匹配；44 个编译后资源均存在。
2. 这些刀的 `m_szAnimClass` 未设置，原生函数回退到 `m_szName`。例如 M9 的动画类是 `weapon_knife_m9_bayonet`，资源目录却是 `knife_m9`；不能通过目录名随意推导动画参数。
3. 524 虽然出现在 VData，但名称是 `weapon_knife_twinblade`，模型/骨架实际别名指向默认 T 刀，本轮不把它当已发布独立刀型开放。
4. 主图 `animation/graphs/viewmodel/viewmodel.vnmgraph_c` 明确包含 22 个刀型分支和相应 `weapon_*` 类型匹配节点。M9/骷髅等使用 `viewmodel_knife` 的 variation；蝴蝶、爪子、锯齿爪刀使用独立子图。**应优先使用原生分支选择，不盲目重建整套手臂图。**
5. `viewmodel_knife` 的 variation 使用 `animation/skeletons/characters/viewmodel.vnmskel`，不是 `weapons/knife_*.vnmskel`。前者是手臂视角骨架，后者是刀身骨架，不能互换。
6. 已确认图参数类型：`action`、`attack_type`、`weapon_type` 为 ID；`action_reset` 为 Bool；`deploy_variation`、`inspect_variation`、`attack_variation` 为 Float。旧版固定 sequence 0 和攻击整数 1/2 不能当作这些参数的实现。

### 字段归属

以下偏移只记录当前 client.dll 的证据。探针尽量从 Schema 取偏移，未安装固定 RVA 写入 Hook。

| 对象 | 字段/证据 | 当前偏移或 RVA |
| --- | --- | --- |
| C_BaseEntity | m_CBodyComponent / m_pGameSceneNode | 0x30 / 0x330 |
| C_BaseEntity | m_nSubclassID / 原生 VData 指针 | 0x380 / 0x388；后者来自机器码，不是命名 Schema 字段 |
| C_BaseEntity | m_nCreationTick | 0x5D0 |
| CBodyComponent | m_pSceneNode | 0x8 |
| CBodyComponentBaseAnimGraph | 内嵌 m_animationController | 0x510，**不是实体 + 0x510** |
| CBaseAnimGraphController | m_hSequence / m_flSeqStartTime | 0xB0 / 0xB4，仅记录、不写入 |
| CBaseAnimGraphController | m_pGraphInstanceAG2 包装对象 | +0x448 为 owner，+0x450 为真实 graph |
| CBaseAnimGraph | m_pMainGraphController | 0x1048 |
| CCS2WeaponGraphController | m_weaponType | 0xE8，ParamRef 对象，不是可直接赋值的 int |
| CCSWeaponBaseVData | m_szAnimSkeleton / m_szName / m_szAnimClass | 0x528 / 0x720 / 0x870 |
| 原生武器图更新 | graph controller + weapon | RVA 0x8E5390 |
| HUD 调用者 | 从 owner pawn 解析实际 weapon，再更新 main graph controller | RVA 0x104E310，调用点 0x104E414 / 0x104E438 |
| main graph getter | 读取实体 + 0x1048 | RVA 0x90FB70 |
| body controller getter | scene node 的虚函数取得 skeleton，再读 skeleton + 0x400 | RVA 0x910120；不要将这个控制器与 main graph controller 混用 |
| 原生 SetModel 字符串包装器 | 内部转换模型名称，再进入模型更新 | RVA 0x934CA0，内部 0x934CE0 |

原生 `0x8E5390` 从 weapon + 0x388 取 VData，再取 AnimClass/Name，经 `m_weaponType` 的 ParamRef 更新 ID 节点。**直接用自造 C 字符串替代原生 ID/symbol 表示仍未验证**，也不能全局修改共享 VData，否则会影响其它玩家。

## 独立事务与性能约束

- 身份包括 XUID、Demo 场景 epoch、pawn/world/HUD/arms 的指针、完整句柄（含 serial）、creation tick，以及图实例。只用实体序号不够。
- 拒绝 HUD 刀身与 arms/world/pawn 别名，拒绝把实体地址当动画控制器地址。
- 探针从真实 weapon services 重读 active handle，不接受“背包中唯一一把刀”冒充持刀状态；要求真实第一人称 observer/pawn 对应关系。
- 探针最多每 250ms 运行一次；HUD 遍历限制为 64 个场景节点，不扫描所有实体、不查询模型资源、不休眠。
- 模拟原生适配器验证了：稳定状态只 Apply 一次，校验最多每 500ms 一次；失败后同一请求/生命周期不重试。
- 模型、动画分支和刀身骨架必须作为一对提交与校验。回退先于新请求 Apply，分两个 tick 进行。
- 成功重建图后接纳经校验的新 graph identity；不是所有图指针改变都意味着外部实体重建。
- seek、实体被释放或句柄/creation tick 重用时，放弃旧实体地址，不向新占位者写旧快照。
- 原生 adapter 将来必须持有必要资源引用、重新验证对象并完成精确回退。不得回写整个 CEconItemView 覆盖用户刚更换的涂装，也不得把刀模型应用到手臂上。

## 后台验证

以下为首次独立状态机阶段的历史记录；v0.4.3 后恢复开发的验证另见上方续作检查点。

- 新增 10 组刀模测试，覆盖请求隔离、22 刀型元数据、11 类禁止写入条件、目标切换、玩家切换、句柄/creation tick/图实例重用、seek、部分失败、失败回退锁定、收刀与能力丢失、图漂移、原模型无操作和原生图重建。
- 100,000 次纯状态机轮询：1 次 Apply，200 次 Verify（模拟 100 秒时间）。耗时约 1.5ms；**这是模拟器开销，不是 CS2 帧率测试**。
- 中英文开发 DLL 编译成功；保持 `/MT` 和内嵌 DXBC。
- 两种语言的隔离 LoadLibrary、System32-only PATH、冲突 D3DCompiler 环境共 6 项通过；无窗口 WARP 渲染检查通过。测试命名中的 HLAE-loadlibrary 表示模拟相同 Win32 加载调用，不代表运行过 HLAE。
- 完整本地回归 43 项通过（原有 42 项 + 新刀模测试组；依赖显式游戏目录参数的可选实机检查未启用），新刀模组内包含上述 10 组细分测试。结果记录在 `build/temp/knife-model-development-status-20260831.md`。
- 本轮未启动 CS2，没有注入或操控桌面，没有录制或上传 Demo 截图。

## 下一阶段：尚未完成的部分

1. 在离线 Demo 中用当前只读探针记录 M9、骷髅、蝴蝶等的真实 pawn → HUD arms → HUD weapon → controller 对应关系，确认生命周期和 body controller 关联。
2. 找到并验证目标刀型的原生 VData/ID symbol 读取方式；只读取目标元数据，不修改共享武器数据。确认 `weapon_type` 的值和原生更新覆盖时机。
3. 将模型设置、刀身骨架/挂点和原生动作分支选择接入事务适配器，限制在已确认的游戏线程时机。不得直接把静态 RVA 调用标为 verified。
4. 实测切刀、轻/重攻击、检视和特殊检视，覆盖 M9 ↔ 骷髅、M9 ↔ 蝴蝶/爪子等跨动作族；验证收刀、换观战玩家、拖动 Demo 和重复切刀的回退与性能。
5. 同时测试枪皮、刀涂装、手套保持不变，以及换模后更改涂装不会重启模型链。通过之前不发布“刀模已可用”的新 Release。
