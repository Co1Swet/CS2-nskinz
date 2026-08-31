# v0.4.4 stock-weapon audit / 原皮枪械修复审计

2026-08-31. Scope: ordinary firearm finishes only; no new knife-model writes.

## Evidence / 证据

The reported stock AWP BOOM request had the correct weapon/paint pair and legacy mesh, with a verified first-person render object. Entity fallback values reported paint 174, but item-view paint/seed/wear records were absent. The global refresh function returned without creating a visible finish. This differed from the earlier M4A4 mesh/HUD failure.

On CS2 Client/Server 2000899, Patch 1.41.7.8, Steam Build 25000182, Source revision 10948930:

- The native refresh loop skips an item when both composite-material lists are empty.
- Native material generation reads paint from the item-view attributes; fallback entity readback alone does not prove the generator receives a paint.
- The native clear/generate call window is decoded and validated against module bounds, executable addresses and both function entries. No instruction-offset patch or fabricated material counts are used.

因此修复同时补齐原皮物品缺失的属性，并为“空材质容器”调用引擎首次生成；仅修改 fallback 数字或重复运行原刷新循环不足以解决。

## Implementation / 实现

- Validate both attribute lists before constructing at most three missing engine-owned paint/seed/wear records. Preserve allocator/owner/vtable metadata; never install an external vector. Existing missing sticker/keychain attributes are not silently accepted.
- Track full entity identity, creation generation, vector identity and the owned tail. Repeated updates reuse records. Native erasure is permitted only for a still-owned range on the verified game frame thread; abandoned/stale generations are not restored.
- Gun-only batches clear/generate only the requested carrier. Mixed batches retain the existing non-gun refresh path and bootstrap a still-empty standard-weapon owner as needed.
- A positive finish requires a valid nonempty material owner before completion is cached. Failure is latched until Update or generation change. Container readback is explicitly not pixel acceptance.
- Existing gun compatibility, mesh selection and carrier/HUD-generation checks remain in place. Knife/glove model and animation logic is unchanged from v0.4.3.

## Validation / 验证

The user confirmed the stock AWP → BOOM demo result through the DLL panel. No demo screenshots or raw user logs are included in this release.

- Nine targeted test entries passed in 0.312 s; all 46 full entries passed in 127.891 s.
- Engine constructor/mutation metadata, 1,000 repeated applications, native generation fixtures, cancellation, missing schema operations, stale entities and bounded retries are covered.
- The actual installed client is mapped read-only into the test process without executing its imports, DllMain or native functions for binding validation.
- Catalog validation: 34 guns / 1,449 compatible pairs, 1,408 weapon paints / 74 glove paints. This is not 1,449 visual demo tests.
- Both exact release DLLs passed six isolated loader cases and offscreen ImGui/WARP rendering across three resource recreations. Static `/MT`, inbox-only direct imports and embedded shaders are retained. No independent clean VM or exhaustive FPS test is claimed.

使用已构建的测试程序可运行 `--weapon-paint-only`；完整回归不带参数。`tools/verify_awp_stock.py` 接收测试程序、本地 CS2 目录、已提取的 items_game/localization 文件与输出目录，后台执行两套检查，不启动或控制游戏。它是开发验证脚本，不是 DLL 的运行依赖。

## Release source boundary / 发布边界

Compared with v0.4.3, product changes are limited to both Visual Studio project files, `src/Cs2/Runtime.cpp`, `src/Cs2/Runtime.hpp`, `src/Tests/Main.cpp`, and the new `src/Runtime/WeaponMaterialInit.hpp`. Documentation and the targeted test runner accompany them. All product source/third-party/project bytes in the release source tree are checked against the isolated build input.

未发布的 KnifePair / KnifeUpdateProbe 续作及新的刀模分析进度仍保留在本地暂停点，不纳入 v0.4.4。
