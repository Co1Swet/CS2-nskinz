# v0.4.3 原皮 AWP → 嘣：缺少首次材质初始化

状态：用户已确认原皮 AWP → 嘣实测成功，随后要求发布 v0.4.4；中英 DLL 已原样发布，提交 `f0f693f0c165089a3642f3f4604c67d247979283`。后台验证以 `build/awp-stock-fix/VALIDATION.md` 为准；此确认不等于所有枪型/涂装组合都已完成画面验收。

## 暂停与范围

用户确认朋友使用 v0.4.3。刀模进度已保存于 [刀模事务暂停点](knife-model-checkpoint-20260831-transaction.md)，另有本地源码快照 `build/checkpoints/knife-paused-before-awp-20260831/source-checkpoint.zip`（SHA-256 `a873551fc79eb462863eb2911b994c9089c1e743e4df6a636675a6249e3e1027`）。

枪械修复在当前源码落地，并把该暂停点之后的枪械补丁单独应用到正式 v0.4.3 源码副本 `build/awp-stock-fix/source`。交付 DLL 从此隔离副本构建，不含未发布的 KnifeGraph / KnifePair / KnifeUpdateProbe 续作。首次交付时未改 GitHub；用户验收授权后，新建 v0.4.4 标签及 Release，旧版本不覆盖。对应发布源码在 `.tmp/publish-v0.4.4/source`，附件位于 `build/v0.4.4/release`，远端核验回执为 `.tmp/publish-v0.4.4/receipt.json`。

## 证据

朋友提供的运行日志 12:29:26：

- XUID `76561199123336549`，武器 definition 9（AWP），entity 1831。
- `paint=174`、`fallback_paint=174`；静态/动态涂装回读为 `attribute_paint=absent`，种子和磨损属性也 absent。
- world/HUD 均 `mesh_mask=2`，HUD 为已验证 render-only 对象。因此不能再次归因于 M4A4 修复前的“HUD 未绑定”。
- 原生刷新函数返回，日志本来就注明 `visual result not verified`；截图仍为原皮。
- 更早的 paint 819 还含 8 个贴纸属性，原地同步失败并回滚。后来的 174 已去除贴纸载荷，不能用前段贴纸失败解释后段 BOOM 不显示。

本机存在 `game/csgo/131331331.dem`。本机目录文件确认 174=`hy_blam_simple`、中文 `*嘣*`，与 AWP 兼容，使用旧版网格。该文件存在并不等于已经进入此 Demo 验收。

相同客户端基线的本地 PE 证据（Client 2000899 / Patch 1.41.7.8 / Steam Build 25000182）：

1. `client+0x7FF0F0`：刷新循环检查 `entity+0xAA8` / `+0xAC0` 的已有复合材质数量；两者为空直接跳过，既不清理也不生成。原皮首次换肤缺少这条入口。
2. 循环内 `0x7FF10B → 0x1439FF0(owner, true)` 为引擎材质清理，`0x7FF115 → 0x7D97F0(entity, false)` 为引擎生成入口。生成器本身接受空 owner，且拒绝非空 owner；不能伪造数量来骗过循环。
3. `C_EconItemView` 主虚表 `+0x18 → 0x110A2A0` 获取涂装，经过 `+0xE8 → 0x1118D60` 遍历物品属性。缺少涂装记录时返回 0；在这条实际生成链上不能把 entity fallback 数字回读等同于有效 paint。

## 修复设计

- 仅标准枪械正数涂装：验证两份属性列表后，通过 Schema 操作 4 构造缺少的 6/7/8（涂装/种子/磨损）记录，通过操作 2 通知再写标量。保留引擎构造的 vtable、owner 和分配器状态，不使用外部向量、不复制节点元数据、不改 provider/registered item ID。
- 不改变现有动态属性同步规则；已有记录不重复追加。不存在的贴纸/挂件记录仍拒绝应用，不能把它们冒充 fallback 已成功。
- 新建属性独立记录所属实体完整句柄、identity、创建 tick、向量及尾部范围。正常取消时仅在身份、位置和当前值仍归本次修改所有时使用引擎操作 5 撤销；seek/销毁、非游戏线程或竞争写入时不回写旧地址。
- 标准枪械单独按目标调用已解码的原生清理/生成器：空材质直接初始化，已有材质先由引擎清理并确认空再生成；保持原模型及 world/HUD 网格逻辑。刀/手套代码不走这条新入口。
- 调用仅限已验证 `FRAME_RENDER_PASS` 游戏线程、已有离线运行时门槛和已验证 carrier generation。校验完整 42 字节调用窗、两端函数入口、模块范围与 ABI。
- 正数涂装生成后必须读到有效非空材质 owner，才允许记录刷新成功；该条件依旧不等于像素验收。失败锁住当前实体/外观，直到 Update 或实体 generation 更新，避免每次轮询反复重建。

## 验收方式

重启 CS2，不叠加旧 DLL，通过 HLAE `-insecure` 加载修复候选。在 Demo `131331331` 中用 DLL 面板给相同 XUID 的 AWP 选择“嘣”，Update 后播放。应看到实际涂装，并在新日志中出现 `Standard-weapon native material initialization` 的 `paint=174`、有效 after 数量、`material_created=1`。再检查切刀切回、换一把合法 AWP 皮肤、取消/无涂装、seek。

HLAE 开发基线 2.191.1；朋友日志未读出 HLAE 文件版本，不能声称已独立核对朋友的 HLAE 版本。用户日志与截图仅用于本地诊断，不加入 GitHub 展示素材。
