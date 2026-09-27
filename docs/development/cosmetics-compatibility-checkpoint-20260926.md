# 换肤进度与 CS2 2000918 / HLAE 2.192.5 适配检查点

日期：2026-09-27

## 本次基线

| 项目 | 已检查值 |
| --- | --- |
| HLAE | `2.192.5`（文件版本 `2.192.5.0`） |
| AfxHookSource2 | `0.41.5`；HLAE changelog 标注适配 CS2 `1.41.8.5` |
| AfxHookSource2.dll SHA-256 | `3C8F04B6CBAE2CF465E37B1D934786F3C1453D984F15A8A1A4C52830CBE79F29` |
| CS2 Steam Build ID | `25537370` |
| Client / Server | `2000918` |
| Patch | `1.41.8.5` |
| Source revision | `11039926` |
| 游戏版本时间 | `Sep 25 2026 13:20:50` |
| client.dll | `39,182,488` bytes；SHA-256 `9B4F46DBD6A433163B39D7EA0123C321B1AD6D95CEEDD40AE121312464833549` |
| animationsystem.dll | `9,329,816` bytes；SHA-256 `7DD8030CD53880F6364AA0C7B9FABCB3770D26A9349C2E1655DF3F062FE71539` |
| engine2.dll | `6,828,696` bytes；SHA-256 `B63B2AB7CAE8115E9BF05562E3289E2AA9E65D3798633CC160B653BC55EE6B52` |

## 当前换肤实现边界

### 枪械

- 保留原有 nSkinzCS2 运行时路径：Schema 字段、引擎原生 `CEconItemAttribute` 追加、涂装／seed／wear 回读以及原生复合材质清理和生成。
- 没有改成 Insight 路径，也不经过临时 Demo 改写器。
- 本次只同步 CS2 把复合材质所有者从 `entity+0x608` 移到 `+0x610`、主／伴随向量从 `+0xAA8/+0xAC0` 移到 `+0xAB0/+0xAC8` 的布局变化；清理和生成函数仍由原来的字节验证解析器取得。

### 刀模型与动作

- 独立实现，不调用、加载或借用 Insight 客户端／`skin-core.exe`。
- 临时换肤 Demo 仍采用“第一次加载捕获配置 -> 生成新的只读来源副本 -> 第二次 `playdemo`”流程；原 Demo 不覆盖。生成结果包含刀定义、子类／模型和动画资源数据，枪皮肤不进入该改写路径。
- 运行时备用链已经迁移到 2000918：动画资产序列化、SetModel、模型通知、Recollect、模型骨架引用、weapon-type 参数图、手臂外层更新边界、HUD 材质传递及刀材质回调均使用新版 RVA／字段，并在调用前匹配完整指令窗。
- `CCS2WeaponGraphController::m_weaponType` 从 `0xE8` 移到 `0x120`，其图和节点索引随之为 `0x128/0x130`；模型骨架计数／引用移动为 `0x440/0x448`。
- 严格文件测试已用本机最新 `client.dll` 和 `animationsystem.dll` 通过，包括 484 个受支持刀定义有向组合的事务回归。该测试证明 ABI 和失败关闭门控正确，不等同于实际 Demo 中所有首次切刀、检视、掉落和 HUD 交接已经画面验收。

### 默认 T/CT 手套

- 权威输入仍是 Pawn 内嵌 `m_EconGloves`，而不是伪造 Wearable；特殊手套的兼容 fallback 与默认手套事务分开。
- 新版 Schema：`m_bNeedToReApplyGloves=0x176D`、`m_EconGloves=0x1770`、`m_nEconGlovesChanged=0x1D20`。
- 原生可视输出组件从 `Pawn+0x1518` 移到 `Pawn+0x15F8`；对应原生刷新入口、client-only identity chunk root 与 HUD consumer 已全部迁移。
- 七个默认手套原生／HUD 指令窗已加入已安装 PE 严格测试并通过。实际 Demo 中默认 T 手套的模型、涂装与重载持续性仍需画面验收。

## 验证结果

执行：

```text
cmd.exe /d /c run-build-and-tests.cmd
nSkinzCS2.Tests.exe --cs2-root "E:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive"
set NSKINZCS2_KNIFE_CLIENT_DLL=...\client.dll
set NSKINZCS2_KNIFE_ANIMATION_DLL=...\animationsystem.dll
nSkinzCS2.Tests.exe --knife-model-only
```

结果：

- Release x64 DLL 与测试程序构建成功；全量原生回归退出码 `0`。
- 本机兼容清单全部 16 项唯一命中：14 项换肤／运行时必需门控，以及骨架模型绑定、地图 GameSession 解析器两项参考证据。
- `knife_graph_installed_evidence=PASS`。
- `glove_visual_installed_evidence=PASS`。
- 新构建 v0.5.2 Beta 英文 DLL：`4,336,640` bytes，SHA-256 `B0D7AE1BF7B634CBE766473332AC719849AEEF3F6E2A45BE0C1218163EF4070B`。
- 新构建 v0.5.2 Beta 简体中文 DLL：`4,338,688` bytes，SHA-256 `CB8CAB8B9726B9A1AAA263AABF5D6793C159E9C481AF5C50CD0EDA05AE5C08B4`。
- 两份 DLL 均通过 HLAE 风格、仅 System32 和不兼容编译器目录三组隔离加载检查；内嵌 DXBC、WARP 10.0、ImGui 像素回读及三次设备重建也通过。该检查不代替 HLAE/CS2 实际 Demo 画面验收。
- 测试过程只映射／读取游戏 PE，没有启动游戏、调用游戏函数或修改游戏文件。

## 下一步实际验收

1. 完全退出旧 CS2/HLAE 进程，用 HLAE 2.192.5 `-insecure` 启动，且只加载一份新 DLL。
2. 第一次加载 Demo 后选择刀与默认手套，生成并自动加载临时 Demo。
3. 第二次加载检查刀的首次 deploy、inspect、切回枪再切刀，以及 T/CT 默认手套模型和涂装。
4. 若失败，保留 `nSkinzCS2.log`、生成 Demo 路径、XUID、刀／手套定义 ID 和失败发生的 tick；不要叠加加载旧 DLL。

## 同版本地图替换兼容补丁

- 对比 MulNX 最新公开源码提交 `f2466e18e26a8fc3daf4ac7945bc45220ba95993` 后，移植其新版 `CSVCMsg_GameSessionConfiguration` 公共字符串出口特征：相对跳转位移使用掩码，尾部以 field 10 分支固定指令约束，避免 Valve 增减 protobuf 字段时仅因位移变化失配。
- 本机 CS2 2000918 的 protobuf 描述符确认 `s1_mapname` 仍是 field 11／`_has_bits_` bit 2，但 `CUtlString` 槽已从 `message+0x28` 移到 `message+0x40`；运行时替换同步迁移。
- 上游掩码特征已对本机 `engine2.dll` 只读扫描，唯一命中 RVA `0x17D153`；地图安装链的全部 11 条 PE 特征均在当前 `engine2.dll`／`client.dll`／`resourcesystem.dll` 中唯一命中，`tier0.dll` 也仍导出 `KeyValues::SetString`。
- 地图代码继续遵守 MulNX 的 AGPL-3.0 许可和项目现有来源声明；未引入 MulNX 文件改写器，也未改变枪械、刀或手套换肤路径。
