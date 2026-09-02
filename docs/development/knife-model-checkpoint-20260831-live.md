# 刀模续作检查点：M9 / 骷髅刀只读实测

日期：2026-08-31。接续 [动画图检查点](knife-model-checkpoint-20260831-graph.md)。本轮完成真实图归属修正和副骨架关联验证，**尚未启用换模写入，不是换刀模型成功版**。

## 实测范围与结果

用户自行打开并切换 HLAE 离线 Demo；助手仅使用外部 `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ` 读取。没有注入 DLL、安装新 Hook、写游戏内存、调用游戏函数、重启游戏或操作桌面。没有采集或上传游戏截图。

基线仍是 CS2 Client 2000899 / Patch 1.41.7.8 / Steam Build 25000182，HLAE 2.191.1。两个实际 CS2 进程的命令行均含 `-insecure`。用户切换后的进程 PID 已改变，重新枚举并采集地址；没有复用旧进程地址执行操作。

| 现场 | 刀型 | 生产 C++ 图参数验证 | 生产 C++ 副骨架验证 |
| --- | --- | --- | --- |
| 初始持刀视角，XUID 76561199032006224 | M9，508 | `weapon_knife_m9_bayonet`，index 1 / count 457，通过 | `active_hud_model_weapon`，slot 0，完整 HUD 句柄 3139289908，通过 |
| 用户切换后，XUID 76561198041683378 | 骷髅，525 | `weapon_knife_skeleton`，index 1 / count 457，通过 | 同名槽位，slot 0，完整 HUD 句柄 1254638308，通过 |

其它未激活的视角虽有 knife/HUD/graph 对象，但 `weapon_type` 为空、副骨架列表为空；没有计为验证成功。M9 旧解析器运行结果为 6 个候选全部 `GraphMismatch`；修正后当前 M9 通过，另外 5 个空参数候选被正确拒绝。切换后的骷髅刀共 9 个候选，仅实际激活视角通过。

原始只读发现数据位于 `build/knife-pair-dev/live-*.json`。最终骷髅刀生产代码结果在 `live-switched.production.json`。这些含运行期地址和玩家 ID 的调试数据保留本地，不进入发布包。

## 关键修正：Schema 字段是包装对象

上一阶段将 `CBaseAnimGraphController::m_pGraphInstanceAG2`（Schema +0x448）直接当作图指针，真实进程证明这一解释错误：

- controller +0x448：包装对象保存的 **controller 自身指针**。
- controller +0x450：实际图实例指针，与 `CCS2WeaponGraphController::m_weaponType` ParamRef +8 指向同一图。
- 构造指令 client `0x8F8CF0` 保存 self 到 +448，随后将 +450 清零。
- client `0x37ADC8` 调用 body getter `0x9100F0`，再从返回对象 +450 取图用于更新。

生产新增 `ReadKnifeBodyGraph`：先检查原生指令和 Schema，再双读包装 owner/graph；拒绝空、owner 错配、self-as-graph、溢出和读取中变化。`ProbeKnifeWeaponType` 同时核对 owner 和真实 +450 图。

**禁止为了让旧读取通过而删除实体/控制器别名检查。** 旧值等于 controller 恰好说明读取了包装 owner，并非应放宽验证。

## 新闭合的原生副骨架链

当前激活视角：arms controller 的 animation algorithm 为 2，拥有图；HUD 刀身 controller 的原始 algorithm 字段为 1，图为空，secondary master count 为 1。刀身由手臂的副骨架关联驱动，不应强行为它构造另一套独立 arms 动画图。

Schema / 原生机器码相互印证：

| controller 字段 | 布局 | 元素 |
| --- | --- | --- |
| m_vecSecondarySkeletonSlotIDs | count +20，data +28，capacity +30 | **8 字节原生 symbol**，不是 int/hash |
| m_vecSecondarySkeletons | count +38，data +40，capacity +48 | **4 字节完整实体句柄**，不是指针 |
| m_nSecondarySkeletonMasterCount | +50 | HUD 刀身的被驱动关联计数 |

活跃槽名实读为 `active_hud_model_weapon`。句柄包含 serial，解析到对应 `C_CS2HudModelWeapon`，owner 指向当前世界刀。

`ProbeKnifeSecondarySkeleton` 已接入独立模型探针：检查两列表数量/容量、不同元素步长、唯一槽位、唯一 HUD 句柄、serial、HUD controller wrapper owner 和单一 master；上限 64 项，双读，失败清空输出。它不写句柄、计数或图，也不枚举所有资源。此校验只在源刀型图参数已验证后运行。

静态原生调用链（仅证据，不是已获验证的可调用适配器）：

1. HUD arms 更新中的 `0x104DDCE` 调用 `0x1046550` 取得活跃 HUD 武器；`0x104DDD9` 调用 `0x9100F0` 取得 arms body controller。
2. `0x104DE1D` 调用 `0x935AE0(controller, &slot_symbol, hud_entity)`；包装器进入 `0x935B00(controller, count, slots, entities)`。
3. `0x935B00` 管理两张并行列表、旧/新 HUD controller master count，并在实际变化后调用 `0x9040C0`。
4. `0x9040C0` 根据副骨架实体的**模型资源**收集骨架引用；`0x1655A80` 读 model +430 count，`0x16559E0` 从 model +438 的 0x50 字节元素取引用。
5. `0x904322` 把收集结果传入 `0x12628A0`：更新图 +338 的引用列表、下游 +100 对象，并设置 +334 标志。

单纯对同一 slot/同一 HUD 实体再次调用 `0x935AE0`，存在“未变化”早退，不应假定换了模型后一定重新收集骨架。需要继续验证模型变更通知能否到达 `0x9040C0`，或怎样在已确认时机触发原生重收集。

## 模型自己拥有骨架资源

`weapon_knife_m9.vmdl_c` 的 DATA `m_vecNmSkeletonRefs` 明确引用 `animation/skeletons/weapons/knife_m9.vnmskel`。进一步用本地 VPK + VRF 对全部 22 个刀模型核对，**每个模型的该字段都恰好对应自身 VData 中的刀身骨架路径**，全部通过。

新增 `tools/verify_knife_model_skeleton_refs.py`，报告在 `build/knife-pair-dev/model-skeleton-refs.json`。提取文件只作临时开发分析，报告只含路径/哈希，不分发 Valve 资源。VRF/Python 不成为 DLL 运行依赖。

因此优先研究的成对路径是：原生 SetModel 更新 world/HUD 刀身资源 → 原生模型通知与副骨架重收集 → 原生 arms 图正确刀型分支。**不需要先假定必须改共享 VData 或把刀身骨架换到手臂。** 此结论是实现方向，不等于已完成写入验证。

## 动画时机的进一步排除

client `0x104E44B` 的 controller 虚函数 +D0 对应 `0x436AF0`：当 secondary master count >=1 返回 3，否则返回 algorithm 字节。**这是算法选择查询，不是图求值。** 不可拿它当“已执行动画更新”的证据。

随后 HUD 路径进入 `0x37ADA0`，从 +450 取图后调用 `0x12603B0 / 0x1260620 / 0x1260460`。与 `0x8E5390` 武器参数更新的具体生命周期/线程同步仍须验证；尚未安装这一回调。每帧重设模型、固定 sequence 0、低频轮询只改一次 weapon_type 均未重新启用。

## 代码、测试与开发产物

- `src/Runtime/KnifeGraph.hpp/.cpp`：真实图包装解析 + 副骨架只读检查。
- `src/Cs2/Runtime.cpp`：独立探针接入，7 段精确指令窗和相关 Schema 门槛；模型适配器仍只读。
- `src/Tests/KnifeGraphTests.cpp`：9 组细分测试，包含真实 PE 逐字节破坏/截断检查、包装漂移、8/4 字节步长、多 master、重复槽/句柄和 serial 重用。
- `tools/knife_live_probe.py`：Schema 名称提取和有界外部只读发现；固定 native 布局只接受当前 client SHA-256。
- `tools/KnifeLiveProbe.cpp/.vcxproj`：直接链接生产 KnifeGraph / KnifeModel 模块进行外部只读验证，不使用另一份 Python 模拟器冒充 DLL 解析测试。
- 最终定向刀模、8 项枪械与完整 46 项回归通过；完整回归耗时 97.594 秒，见 `build/knife-pair-dev/validation-final/report.json`。
- 中英文开发 DLL：`build/knife-pair-dev/en/nSkinzCS2.dll` 和 `build/knife-pair-dev/zh-CN/nSkinzCS2.zh-CN.dll`。保持 `/MT` 与内嵌 DXBC，仅新增独立刀模诊断；不作为正式换模包分发。
- 6 项隔离加载和无窗口 WARP 渲染测试通过，见 `build/knife-pair-dev/portability.json`；不是纯净机器或游戏内涂装回归。产物哈希见该目录的 `VALIDATION.md`。

v0.4.3 DLL、标签、源码 Release 包未改动，没有 GitHub 写操作。

## 下次续作入口

1. 查清 SetModel `0x934CA0 → 0x934CE0` 模型变更通知对父 arms 副骨架引用的刷新：不能以相同 slot 重新登记早退冒充刷新成功。
2. 验证新模型、对应骨架引用的持有/释放，以及提交/失败/恢复过程中各实体与图实例的生命周期。
3. 在原生 weapon_type 更新后、真正图更新前验证回调；目标 symbol 使用游戏已有全局符号，不写自造字符串、不改变共享 VData。
4. 完成独立 Capture / ApplyPair / VerifyPair / Restore 原生适配器，再通过 nSkinz 面板进行换模、切刀、轻重攻击、检视/特殊检视、seek 和跨玩家实测。
5. 回归刀饰面、枪皮和手套独立性、材质稳定性与帧时间。当前两种刀的只读通过不能替代这些检查。
