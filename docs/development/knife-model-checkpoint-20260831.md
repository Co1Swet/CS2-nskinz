# 刀模开发暂停点：2026-08-31

因用户要求，暂停刀模开发，优先排查朋友机器上 M4A4「破浪狂飙 → 皇帝」变原皮的问题。本文件用于恢复刀模工作，不代表换模功能已经完成。

后续：M4A4 修复已随 v0.4.3 发布；恢复刀模开发后的新增代码、节点表边界证据与验证见 [动画图参数续作检查点](knife-model-checkpoint-20260831-graph.md)。下文保留暂停时的历史状态。

## 已保存的实现

- 主记录：`knife-model-chain.md`。独立事务在 `src/Runtime/KnifeModel.hpp/.cpp`，对应测试为 `src/Tests/KnifeModelTests.cpp`。
- `src/Cs2/Runtime.cpp` 的 `PollIndependentKnifeModel` / `KnifeModelReadOnlyAdapter` 只读接入；饰面走独立 `BuildKnifeFinishRequest`。尚未开启原生模型、骨架或动画写入。
- 上一阶段开发 DLL 在 `build/knife-model-dev/{en,zh-CN}/`；正式 `build/Release` v0.4.2 未覆盖。没有发布新的刀模 Release。
- 本次继续分析只新增/扩展开发工具：`.tmp/knife_model_audit.py` 的导入、导出和 IAT 交叉引用；`tools/NativeSymbolProbe.cpp` 与 `tools/KnifeNativeTests.vcxproj`。本次未完成新的生产动画参数模块。

## 本次补充证据

分析基线仍为 CS2 Client 2000899 / Patch 1.41.7.8 / Steam Build 25000182。重新核对 client.dll SHA-256：`B8E2C009763E8CEFB88D89A2BDCF452DB17501553D473DB6DA060DF8E6769EB4`。

1. `CCS2WeaponGraphController` 构造函数 RVA `0x8CFCB0`：`weapon_type` 名称字符串位于 `0x1ACEF68`；`0x8CFDC5` 经 IAT `0x1971780` 调用 tier0 的 `_MakeGlobalSymbol`，不是大小写敏感版本。返回符号指针保存到 controller + `0xE8`。
2. 该 ParamRef 的图指针在 `+0xF0`，有符号 16 位节点索引在 `+0xF8`；初始索引为 -1。这些是 ParamRef 内部字段，不可将 `m_weaponType` 直接当 int 或普通字符串字段写入。
3. 原生武器更新 `0x8E5390` 在 `0x8E54AA` 从 weapon + `0x388` 取共享 VData，优先取 `+0x870` AnimClass，否则 `+0x720` Name；经图 + `0x10` 的节点指针表，最后在 `0x8E54E5` 写 ID 节点 + `0x18`。
4. ID 节点 RTTI 名称是 `CNmControlParameterIDNode`。节点表数量字段仍未证明，不猜偏移。
5. HUD 更新 `0x104E310` 的两个调用点 `0x104E414` / `0x104E438` 会再次执行武器图更新。因此在普通低频轮询中改一次 `weapon_type` 会被原生更新覆盖；后续需要确认原生更新之后、图求值之前的游戏线程时机，不能靠每帧重设模型补救。
6. tier0 的导出已核对：`_MakeGlobalSymbol` RVA `0x1A3090`、`_FindGlobalSymbol` `0x1A3110`、`_FindGlobalSymbolByHash` `0x1A30E0`、`_MakeGlobalSymbolCaseSensitive` `0x1A3260`。产品实现优先只查找已存在的原生符号，不修改共享 VData，也不把自己创建的字符数组地址当原生 ID。

## 已做的独立后台测试

`build/knife-native-tests/nSkinzCS2.NativeSymbolCheck.exe` 在自身进程中加载本机 `game/bin/win64/tier0.dll`，未启动、注入或操作 CS2。

- 22 种刀型全部通过：不同字符缓冲区输入得到相同原生符号指针；Find 与 Make 指针一致；大写查询一致；符号前置哈希可回查；各刀型符号互不混淆；不存在的符号查询返回空。
- 最终输出：`native_symbol_result=PASS; scope=isolated-tier0-process; cs2_not_started=1`，退出码 0。
- 编译记录：`build/temp/knife-native-symbol-build.log` 与 `.err`。
- 该测试只证明符号表示和查找 ABI，不证明游戏中的对象归属、模型资源生命周期、切刀/检视动作或穿模已经解决。辅助进程的指针不能复用于 CS2 进程。

## 恢复时下一步

1. 实现带 Schema/机器码校验的只读 ParamRef 解析，验证名称符号、图归属、索引、ID 节点类型和源刀型符号；避免旧节点地址跨图重建缓存。
2. 在离线 Demo 获取真实 pawn → arms → HUD weapon → graph 归属证据，再选择原生更新之后的成对模型/动画提交时机。
3. 模型、刀身骨架/挂点、动画分支同一事务；不得 SetModel 到 arms，不得覆盖手臂 viewmodel 骨架，不得使用旧 sequence=0，不得覆盖涂装属性。
4. 完成切刀、攻击、检视、收刀、换玩家、seek 及稳定帧时间实测后，才可宣称刀模可用。

参考符号实现：[AlliedModders CS2 SDK CUtlSymbolLarge](https://github.com/alliedmodders/hl2sdk/blob/cs2/public/tier1/utlsymbollarge.h)。当前二进制 ABI 已另行本机验证，不能仅凭参考 SDK 假设游戏布局相同。
