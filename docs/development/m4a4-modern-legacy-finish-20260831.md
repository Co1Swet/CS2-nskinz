# M4A4「破浪狂飙 → 皇帝」涂装丢失排查与修复

日期：2026-08-31。状态：代码修复与后台定向回归通过；朋友的原始 Demo 画面尚未验收。刀模开发已暂停并另存 [分析暂停点](knife-model-checkpoint-20260831.md)。

## 已确认的证据

用户提供的 `nSkinzCS2.log`：

- 02:53:34：M4A4 definition=16、paint=844，原地属性同步 `changed_slots=3, verified=1`，没有替换游戏属性向量。
- 同一时刻世界武器 entity=1898 的 fallback 和 attribute paint 均回读为 844，seed=0，wear=1e-7；材质刷新函数已经返回。
- 同一条记录的 `render_mesh_mask=2`，但 `hud_entity=-1`。所谓 stable state 只证明字段回读，日志自身也标注 visual result not verified；不能用它断言画面成功。
- UI 日志没有显示崩溃，目录包含 1408 个枪械涂装。该故障不是 DLL 没有加载，也不是这一次目标涂装被写成 0。

本机相同游戏基线的 `scripts/items/items_game.txt`：

| 涂装 | ID | 游戏配置 | 网格选择 |
| --- | --- | --- | --- |
| 破浪狂飙 / Full Throttle | 1353 | `m4a4_skrrrt_green`，独立 composite material，未设置 `use_legacy_model` | 现代网格 1 |
| 皇帝 / The Emperor | 844 | `gs_m4a4_emperor`，`use_legacy_model=1` | 旧版网格 2 |

这说明故障发生在新旧模型涂装切换场景。以下代码缺陷已确认，但是否还有该 Demo 特有的材质刷新问题，必须通过同场景视觉复测确定。

## 修复内容

1. **读取每个涂装的模型标志。** `Catalog` 保留 `use_legacy_model`，IPC 快照同时保存 true/false；已解析但未设置该标志的涂装是 modern，不再把所有非零 paint 都当 legacy。旧快照缺少元数据时暂保留原有 fallback，不缩减皮肤目录。
2. **单独解析枪械 AG2 HUD 枪身。** 旧渲染回退要求模型路径包含 knife，无法处理 M4A4。新解析器沿已验证的 arms 子树找唯一 `C_CS2HudModelWeapon`，核对 pawn/world/arms 完整句柄和当前持有武器、owner、scene roundtrip、RTTI 与模型路径；不再靠刀路径关键字或候选评分。
3. **保留已验证的 render-only 对象。** 这类 HUD 枪身可能不在普通网络实体列表，负数编号只是本地记账编号。旧绑定会丢弃它；现在经过验证后保留并同步世界/HUD 两侧网格。
4. **不向 render-only 枪身伪造 CEconItemView。** 属性仍写入世界 econ carrier；HUD 只处理模型网格，不操作枪皮属性、共享 VData 或刀模动作。即使该 HUD 存在实体列表编号，也不把它误当世界属性载体。
5. **不再把缺失 HUD 的字段回读描述为完整材质成功。** 日志区分 world/HUD fields 和 world fields ONLY，并始终标注画面未验证。

场景遍历最多 64 个节点；未识别对象、不同 serial、非当前武器、手臂/世界实体别名、循环和多候选均不做 HUD 写入。没有增加每帧材质重建、等待循环或模型 SetModel 调用。

## 后台测试

- 7 个定向测试入口通过，包含目录 IPC、目录解析、完整本机游戏数据、M4A4 新旧网格切换、原地属性同步、非阻塞网格写入和饰面载荷。
- 真实目录仍为 1408 个武器涂装、74 个手套涂装；1353=modern、844=legacy 经生产目录解析器确认。
- 使用测试进程内存夹具执行真正的 HUD 解析器（包含 RTTI、句柄和场景读取），覆盖未入实体列表的枪身、错误 serial、收刀、arms 重用、多个枪身候选、循环与错误场景归属。
- 实际绑定适配器验证：已验证的负数 HUD 编号被保留；未验证指针和空指针被拒绝；HUD 永不替代世界属性载体。
- 原有网格写入测试 240 次全部通过；这是本地内存测试，不是游戏帧率测试。
- 中英文 `/MT` DLL 编译成功。6 项隔离 LoadLibrary 测试及无窗口 WARP/内嵌 DXBC 渲染检查通过，无新增 VC++/D3DCompiler 运行库依赖。
- 完整回归结果以 `build/m4a4-hud-fix/VALIDATION.md` 为准。

## 构建与交付边界

- CS2：ClientVersion 2000899 / Patch 1.41.7.8 / Steam Build 25000182；HLAE 开发基线 2.191.1。
- 开发 DLL：`build/m4a4-hud-fix/en/nSkinzCS2.dll`、`build/m4a4-hud-fix/zh-CN/nSkinzCS2.zh-CN.dll`。
- 正式 `build/Release` v0.4.2 未覆盖，GitHub Release/标签未修改。
- 没有启动 CS2、注入游戏、操作桌面或上传用户截图/日志。当前结果不等于朋友画面已恢复。

## 同场景验收

重启 CS2，通过 HLAE `-insecure` 离线 Demo 加载修复 DLL，切到同一玩家的 M4A4，用 DLL 面板从 1353 换 844，再从 844 换回 1353。记录截图和新日志，要求：

1. 皇帝与破浪狂飙均实际显示，来回切换不变原皮；种子和磨损设置仍保留。
2. 日志出现 `hud_render_only=1`，HUD 有已验证对象（可以是负数记账编号），皇帝两侧 mesh=2、破浪狂飙两侧 mesh=1。
3. 切刀再切回、换玩家、Demo seek 后仍显示目标皮肤，不增加重复材质刷新卡顿。
4. 若 HUD 仍 unresolved 或画面仍为原皮，需要原始 Demo、配置和新版日志，继续检查真实对象路径/材质生成器；不要再以 paint 数字回读替代视觉验收。
