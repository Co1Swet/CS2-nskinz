# 手套原皮恢复与刀型 subclass 预装检查点（2026-09-08）

本检查点接续 `knife-glove-performance-checkpoint-20260907.md`。用户最近的画面反馈是：特殊刀模型可以出现，但第一次切刀仍使用原 T 方匕首动作，武器选择 HUD 仍显示原匕首；无涂装的 T/CT 默认手套不生效。本轮只进行了源码分析、当前安装文件校验、后台编译和无窗口测试，没有启动、注入或控制 CS2，不能把离线测试表述成 Demo 画面验收。

## 已修正的根因

- 刀的旧链只改 `CEconItemView::m_iItemDefinitionIndex`、模型和动画图，未更新实体自身的 `C_BaseEntity::m_nSubclassID` 及相邻武器 VData。因此原生首次 equip 和 HUD 创建仍会从源 T/CT 匕首身份出发，之后的模型/动画修补只能让第二次切刀看起来正常。
- 当前链在刀仍处于收起状态时，先由 `tier0!CUtlStringToken::Make` 生成目标 `weapon_knife_*` token，再调用当前 `client.dll` 中唯一验证的 `C_BaseEntity::OnSubclassIDChanged`。调用前后均核对完整实体 handle、owner、creation tick、item view、token、VData 与模型路径；失败会通过相同原生回调恢复源 token/VData，并恢复饰面快照和源模型。
- `OnSubclassIDChanged` 的当前指令窗唯一命中 `client.dll` RVA `0xAC0140`，其中 VData 位移 `0x388` 与 Schema 的 `m_nSubclassID=0x380` 加 8 完全一致。该入口已列为必需兼容签名，更新后不唯一时 DLL 会拒绝此链。
- 原皮手套此前先正确解析成 T `5028` / CT `5029`，随后却因模式是 `model_and_finish` 被一律赶到特殊手套 `CEconWearable` 工厂。现在只有“显式无涂装、paint 0、目标定义严格等于当前阵营默认手套”走 Pawn `m_EconGloves` 原生重建；特殊手套仍要求真实 Wearable，不共享该例外。
- 实体重建后的手套后备创建现在以 provider generation、Pawn 完整 handle 和 Pawn 指针共同构成代次。临时空返回不会永久消耗该代次，而按 125/250/500/1000/2000 ms 上限重试；已经发布的实体则按完整 handle、实体指针和 XUID 三者验证后复用，避免重复创建与槽位复用误绑定。

## 当前兼容基线

- Steam Build ID：`25175329`
- CS2 ClientVersion / ServerVersion：`2000905`
- PatchVersion：`1.41.8.0`
- Source revision：`10973549`
- VersionDate / VersionTime：`Sep 07 2026` / `14:36:30`
- `client.dll` SHA-256：`809b62b2397e7849995ea427ed99fe2270d2f8ceae731e5ffa43c271e132f3ae`
- HLAE 兼容接口基线仍为 `2.191.1.0`；本轮没有启动 HLAE 做画面测试。

当前安装态扫描中 14 个声明的签名均唯一命中，包括 Schema、实体系统、属性追加、Wearable 工厂/刷新/SetModel 与新增 subclass 回调。完整测试通过：刀模 11 组、刀图/生命周期 17 组、22×22 共 484 个有向刀型事务、手套路由/重建、枪皮与目录回归。两份 DLL 均为 x64 `/MT`，直接导入仅有 Windows 系统 DLL；6 种隔离加载和无窗口 WARP/内嵌 DXBC 检查通过。

## 开发 DLL

- 英文：`build/knife-glove-subclass-fix-dev/en/Lyan_nskinz v0.4.5 glove-subclass-fix EN.dll`
  - SHA-256：`d0ac4ad3b19b6b6413ba055ecce35ec2e3ba4be15ee40308e19f1d4d579328ad`
- 中文：`build/knife-glove-subclass-fix-dev/zh-CN/Lyan_nskinz v0.4.5 glove-subclass-fix zh-CN.dll`
  - SHA-256：`23512d898c9bd6281885be2d2a4cfc09b547fdbf0598315c3fccf8a529b4331b`
- 隔离加载报告：`build/knife-glove-subclass-fix-dev/portable-load-report.json`

这是延续 v0.4.5 源码的开发候选，不是新的公开 Release，不能与旧 DLL 叠加加载。实测前必须完全退出 CS2/HLAE，再只加载一种语言版本。

## 下一次 Demo 验收

1. 先只启用手套“无涂装”，分别观察 T 和 CT 玩家；seek 或切换回合触发 Pawn 重建后再次确认。预期不创建特殊 Wearable，且每个新 Pawn 代次重新触发一次原生手套重建。
2. 再只启用刀“饰面＋模型”，以 T 默认匕首切到爪子刀：先持枪让目标刀完成收刀预装，再第一次切刀。预期首帧 deploy、检视、模型和 HUD 都使用爪子刀身份。
3. 最后测试 `.drop` / 拾取和一次 demo seek。新 owner 或新实体代次必须放弃旧租约并重新预装，不能把旧玩家的 token/VData 恢复到新实体。
4. 若仍失败，保留新的 `nSkinzCS2.log` 与 `nSkinzCS2.ui.log`。关键行应包含 `Holstered knife pre-armed before native equip`、`subclass_prearmed=1`、非空 `vdata=`，以及默认手套的 `Applied glove slot and queued engine-owned visual rebuild`。
