# v0.4.3 appearance audit / 枪械外观复查

Date: 2026-08-31. Scope: standard gun compatibility, mesh selection, render-only HUD binding and completed-refresh lifetime. Private live snapshots are deliberately excluded.

## Findings and handling / 发现与处理

| Area / 范围 | Finding and fix / 发现与处理 |
| --- | --- |
| Paint metadata / 涂装元数据 | Honor explicit `use_legacy_model`; absence on a parsed paint means modern. Do not apply a blanket legacy mask to every positive paint. / 按实际元数据区分新旧网格。 |
| Compatibility / 兼容性 | Merge exact pairs from repeated `item_sets` and `client_loot_lists`; validate UI, imported selections and actual weapon definitions before any writes. / 重复段合并，三个写入入口统一校验。 |
| HUD discovery / HUD 查找 | The network cache can end below the client-only arms slot. Query the single explicit handle, then verify serial, owner, RTTI and scene links. Keep a bounded 64-node walk. / 不以缓存缺失断定 HUD 不存在，不扩大扫描。 |
| Apply status / 成功判定 | For a watched active gun with an expected arms handle, unresolved HUD means pending before attributes, mesh, keychain, precache or refresh operations. / 不以世界枪身字段正确替代第一人称绑定成功。 |
| Refresh lifetime / 刷新生命周期 | Slot/profile-only caches can outlive an entity. Track full handle, object/identity addresses, opaque creation tick and verified render/HUD identities before reusing success. / 同槽不同代实体或新渲染对象使对应枪械缓存失效。 |
| Nonblocking behavior / 非阻塞 | Preserve epochs across stable bindings and missing optional consumers. Reject partial identity reads without mutating caches. Reset epochs with existing seek/full-reset invalidation. / 不完整读取不清缓存、不重复刷新；不添加等待或逐帧模型重绑。 |
| Knife separation / 刀模隔离 | The new guard is standard-gun-only. Existing knife/glove HUD resolvers already use explicit native entity lookup. The unfinished independent knife-model probe still needs its own high-slot/lifecycle work; its production adapter cannot write. / 未完成的只读刀模探测保留，不纳入本版可用模型功能声明。 |

## Regressions / 回归

- Real installed catalog: 34 guns, 1,449 unique compatible pairs. All pairs retain explicit legacy/modern metadata and remain visible exactly once. Full catalog: 1,408 weapon paints and 74 glove paints.
- M4A4 Emperor 844 / Full Throttle 1353, USP-S Traitor 1040, shared AK-47/Five-SeveN Case Hardened 44, and M4A4/M4A1-S distinction.
- Exact parsing, comments/platform exclusions, unknown IDs, old snapshots, IPC round-trip, Chinese names, No Change versus explicit Vanilla, and no-write rejection at all three production entrypoints.
- Actual HUD walker: sparse/high-slot arms, serial and owner mismatches, inactive other weapons, duplicate candidates and bounded traversal. Actual apply pipeline: pending before writes when required HUD is missing.
- Actual epoch tracker: slot serial reuse, creation tick changes (including stable sentinel ticks), moved allocations, separate render/HUD recreation, absent consumers, 1,000 unchanged polls, invalid back-pointers and wrong slot IDs. Unrelated cached state is preserved.
- Complete native suite (45 entries, including the independent knife-model scaffold), 8 targeted entries, English/Chinese builds, 6 isolated loading cases and headless WARP rendering.

用户确认皇帝案例游戏内成功；缓存保护及其它边界主要由后台测试验证。没有新增桌面控制、游戏注入或整段 FPS 实测。兼容列表不是所有涂装的视觉认证。

The Emperor case is user-confirmed in game. Other edge cases and the generation guard are background-tested, not an exhaustive visual/FPS certification. No desktop control or new game injection was performed for this release audit.

Compatibility: CS2 Client 2000899 / Patch 1.41.7.8 / Steam Build 25000182; HLAE 2.191.1. Existing knife vanilla-material, unfinished independent model/animation and missing-keychain-module limitations remain.
