# 刀模续作检查点：动画图参数验证

> 后续实测修正：Schema +0x448 是 owner/graph 包装对象，真实图指针在 controller +0x450。本文保留为历史记录，最新实现与 M9 / 骷髅刀证据见 [只读实测检查点](knife-model-checkpoint-20260831-live.md)。不要重新采用旧的 +0x448 直接指针读法。

日期：2026-08-31。接续 [原暂停点](knife-model-checkpoint-20260831.md)，基于已发布 v0.4.3 的本地源码继续开发。本阶段不是可用换刀模版本，未发布 Release。

## 已实现

1. 新增 `src/Runtime/KnifeGraph.hpp/.cpp`，独立读取 `weapon_type` ParamRef。接口没有 Write、Call 或 Allocate，不能更改涂装、模型、动画或共享 VData。
2. 接入 `PollIndependentKnifeModel`：先验证当前真实持刀玩家及 pawn/world/HUD/arms 身份，再验证动画图归属、源刀型符号和 ID 节点。目标原生符号未加载会单独报告，不会自行注册符号。
3. 客户端 HUD arms/weapon 超出普通实体缓存时，仅用原生 getter 查询对应完整句柄，不扩容或扫描整个缓存。查询后再次核对句柄和 creation tick，拒绝读取中发生的实体代际变化。
4. HUD 场景树遍历从 arms 的子节点开始，检查 Schema `m_pParent`、owner 完整句柄和 scene 回指，拒绝外部节点、重复候选、环、不可读链和超过 64 节点的截断结果。
5. 已读取的 native creation tick `-1` 与“没有读取”分开；后者使用 `INT_MIN`。还拒绝实体完整句柄重复、图实例与控制器别名。
6. 探针仍限频 250ms，日志只在状态、绑定或请求变化时记录；不在每次切刀重新设置模型、不刷新材质、不扫描资源。

刀模请求仍只包含 XUID/目标刀型。v0.4.3 的枪型—涂装兼容性、M4A4 新旧 mesh 选择和枪/刀饰面/手套模块没有改动。独立链的原生适配器仍为 `KnifeModelReadOnlyAdapter`，`native_pair_adapter_verified=false`、`safe_point=false`。

## 这次闭合的证据

基线：CS2 Client 2000899 / Patch 1.41.7.8 / Steam Build 25000182；HLAE 2.191.1 为既有兼容基线，本次没有启动 HLAE 或 CS2。

| 对象或代码 | 已核对字段 / RVA | 依据 |
| --- | --- | --- |
| weapon_type ParamRef | controller +0xE8 名称、+0xF0 图、+0xF8 有符号 int16 索引 | Schema 与 client `0x8E54AA` 指令窗 |
| CNmGraphInstance | count +0x8、指针表 +0x10；分配大小 0x3D0 | 构造 `0x3AC8B0`、析构 `0x3ADBC0` 互相印证；关键析构指令窗 `0x3ADC57` |
| CNmControlParameterIDNode | definition +0x8、value +0x18 | 构造 `0x475120` 指令窗 |
| ID 节点 definition | 有符号 int16 索引 +0x8 | 原生构造按同一索引取得节点 |
| ID 节点 vtable | animationsystem +0x699238 | 构造中的 LEA；离线 RTTI 为 CNmControlParameterIDNode；类型方法 `0x2D0BA0` 返回 2 |

上述 animationsystem RVAs 不属于 client.dll。代码只将 RVA 用于读取/核对，不将它们作为函数调用地址。vtable 由已加载模块基址加 RVA 得到，不能缓存一次启动的绝对地址。

生产门槛同时检查 Schema `m_weaponType==0xE8` / `m_pGraphInstanceAG2==0x448` 和三段精确指令窗（63/48/55 字节）。任何一段不匹配就报告未验证，不读取内部节点。回归测试对当前磁盘上的 PE 运行同一生产校验器，并逐字节翻转验证拒绝。

参数解析顺序：

1. 名称必须是游戏进程自身符号表里已有的 `weapon_type` 原生指针。
2. ParamRef 的 graph、body controller 的 graph、绑定 graph 必须相同。
3. 先读真实节点数量，要求 `0 <= index < count <= 32768`，检查地址运算溢出，再读取一个目标节点。
4. 节点必须对应已验证的 ID vtable，definition 索引必须一致，value 必须是源刀型的原生符号指针。
5. 重读整个参数样本；可观察到的变化一律拒绝，失败结果不暴露旧节点指针。双读是只读稳定性检查，**不是线程安全写入时机的证明**。

符号只使用当前进程 tier0 `_FindGlobalSymbol`；没有调用 Make，不把普通 C 字符串或辅助进程符号地址写入游戏。

## 原生更新顺序：下一步切入位置

继续核对 client `0x104E310`：从 owner pawn 获取实际 weapon，再经 main graph getter `0x90FB70` 取得控制器，在 `0x104E414` 或 `0x104E438` 调用 `0x8E5390`。随后 `0x104E440` 获取 body controller、`0x104E44B` 调用其虚函数 `+0xD0`。

这一顺序进一步排除了“在低频轮询里单独写一次 weapon_type”方案：后续原生武器更新仍会用源 VData 覆盖。后续应验证武器图更新返回处的对象/线程/生命周期，再证明它与图求值的相对顺序。**尚不能仅凭这些指令认定 +0xD0 就是图求值，或宣称回调是安全提交点。** 本阶段没有安装新 Hook。

## 后台验证与开发产物

- `src/Tests/KnifeModelTests.cpp`：10 组独立事务测试；禁止提交条件由 11 类扩展到 15 类。
- `src/Tests/KnifeGraphTests.cpp`：7 组，包含源符号、图归属、索引边界/溢出、错误节点、图重建、真实生产 HUD 绑定函数的高位句柄/父子链测试，以及磁盘 PE 指令窗验证。
- `tools/verify_knife_graph.py`：可重复的后台测试入口，记录实际测试 EXE 和输入 PE/目录数据的 SHA-256；不启动、注入或控制 CS2。
- 中英文开发 DLL：`build/knife-graph-dev/en/nSkinzCS2.dll`、`build/knife-graph-dev/zh-CN/nSkinzCS2.zh-CN.dll`，保留 `/MT` 和内嵌 DXBC。它们只增加刀模只读诊断，不是换模成功版。
- 6 项隔离加载测试和无窗口 WARP 渲染检查通过，见 `build/knife-graph-dev/portability.json`。不是独立纯净机器或 HLAE 游戏实测。
- 定向刀模和 8 项枪械回归通过；真实目录仍有 34 种枪、1,449 个兼容涂装组合。完整回归 46 项全部通过，耗时 143.25 秒；详见 `build/knife-graph-dev/validation/report.json`。
- 未覆盖 v0.4.3 正式 DLL、标签或发布文件；没有 GitHub 写入，没有控制桌面，没有采集/上传游戏截图。

复跑示例（Python/MSBuild 仅开发测试需要，不是最终 DLL 的依赖）：

```powershell
python tools/verify_knife_graph.py --tests build/knife-graph-dev/tests/nSkinzCS2.Tests.exe --cs2-root "E:/SteamLibrary/steamapps/common/Counter-Strike Global Offensive" --items-game build/temp/m4a4-audit/items_game.txt --localization build/temp/m4a4-audit/csgo_schinese.txt --output build/knife-graph-dev/validation
```

## 尚未完成

1. 通过当前探针取得 M9、骷髅、蝴蝶等在离线 Demo 中的真实图归属及源 `weapon_type` 通过记录。
2. 确定模型资源持有/释放、world 与 HUD 刀身模型、刀身骨架和挂点的成对提交方式；不能把刀骨架放到 arms，不能替换手臂 viewmodel 骨架。
3. 确认原生图更新之后、图求值之前的提交时机，接入事务适配器；不能以旧 sequence=0、每帧 SetModel 或共享 VData 修改替代。
4. 实测切刀、轻/重攻击、检视/特殊检视、收刀、换玩家、seek 和帧时间，再同时验证枪皮、刀饰面和手套未受影响。通过之前不开放或发布“刀模可用”。
