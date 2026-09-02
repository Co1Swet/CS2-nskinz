# 刀模暂停点：原生生命周期开发版（2026-08-31）

接续 `knife-model-checkpoint-20260831-native-port.md`。本次只继续独立刀模链；没有改动或发布 v0.4.4。没有启动/注入 CS2，没有控制桌面。本轮进程检查未发现 CS2，因此没有实际游戏画面、切刀动作或 FPS 成功结论。

## 状态变更

**这是已接入模型写入的实验性 DLL，不再是只读探针。** 只有面板“饰面＋模型”的有效刀型请求进入此链；“仅饰面”不进入。需要 HLAE、`-insecure`、离线 Demo、当前游戏线程及匹配的原生回调。不要将此开发版替换公开 Release。

新产物：

- `build/knife-lifecycle-dev/zh-CN/nSkinzCS2.zh-CN.dll`
- `build/knife-lifecycle-dev/en/nSkinzCS2.dll`
- `build/knife-lifecycle-dev/tests/nSkinzCS2.Tests.exe`
- `build/knife-lifecycle-dev/probe/nSkinzCS2.KnifeLiveProbe.exe`（只读工具，禁止写原语）

源码快照：`build/checkpoints/knife-lifecycle-20260831/source-checkpoint.zip`；SHA256 和逐文件清单见同目录 `manifest.json`。之前 native-port / resources / AWP 暂停点均保留。

## 新接线和限制

1. `KnifeLifecycle` 持有独立的请求、候选绑定、当前事务和失败锁存。请求只有 XUID 和刀型，不包含涂装、磨损、种子、手套或通用配置版本；不会因无关饰面配置更新就主动重载刀模。
2. `KnifeUpdateProbe` 在既有 `8E5390(controller, world)` 之外 Hook `104E310(arms, bool)`。外层入口的 12 字节、直接调用点 `104E282` 的 5 字节，以及两个内部调用点和后续图更新位置均做精确校验。静态再次核对外层 `dl` 参数和调用点返回值未使用。
3. TLS 深度限定最外层匹配调用；只在已登记游戏线程、准确返回地址、当前 arms/controller/world 匹配的原生武器更新之后提交。外层退出后无写入许可；递归回调不能借用外层许可。**这些是代码和磁盘证据，本轮尚无真实外层 Hook 现场计数。**
4. 回调重验完整 handle/serial、创建 tick、scene epoch、当前持刀、pawn/controller/SteamID 双向归属、world/HUD/arms owner 和 scene/body/graph 对应关系。不再要求已知失配的旧 observer relay；该值仍记录为诊断。
5. 原生提交顺序：世界刀模型 → HUD 刀模型 → 收集当前副骨架 → 条件更新原生 `weapon_type` ID。模型路径只取固定规范表。保持 arms 模型、饰面字段、共享 VData 和旧 sequence 不变。
6. 稳定维护核对已提交的模型资源身份、副骨架、挂点、图节点，按需要修正源/目标参数；不逐帧读模型路径、不逐帧 SetModel/recollect。每 500 ms 保留完整验证，候选发现每 250 ms，候选提交有效期 1 秒。
7. 新恢复账本分别记录世界刀与 HUD 的实际触碰和目标资源。收刀、切枪、禁用、请求改变、图重建时尝试恢复仍属于本事务的对象；Gone/Replaced 不同于 Unreadable。不会因 HUD 消失就跳过世界刀恢复，也不会覆写另一写入方替换的模型。
8. 恢复时重新解析当前图，允许旧世界刀消失后仅验证 HUD 副骨架。参数 compare/exchange 后再次确认节点及图身份。部分恢复遇到许可失效时保留“骨架需重收集”状态。失败限制每个角色一次尝试，不无限逐帧重试；未解决的恢复失败阻止新事务。新 scene epoch 丢弃旧地址，绝不跨 seek 重放。
9. 同一外层调用内恢复后不立即再次加载新目标；下一新边界/候选才允许重试。同一失败候选锁存，新的实体/图/epoch 身份才能重新尝试。
10. 独立回调不写日志、不扫全实体表。日志在低频 Poll 输出事务计数和 `callback_avg_us` / `callback_max_us` / `steady_max_us`。这些是回调耗时，不是完整游戏帧时间。

面板使用 `Independent knife model (experimental)` 路径，显示实验性说明和真实事务阶段，不再显示“写入尚未启用”。`Applied` 只代表内部模型/骨架/参数校验，不代表动画画面验收。

## 已验证

游戏基线：Client/Server **2000899**、Patch **1.41.7.8**、Steam Build **25000182**、Source revision **10948930**；本机 HLAE **2.191.1**。仍按安装文件证据校验，不保证之后游戏更新兼容。

- client.dll SHA256：`b8e2c009763e8cefb88d89a2bdcf452db17501553d473db6da060df8e6769eb4`
- animationsystem.dll SHA256：`eec896282d13ee0d3c83a628c7e58b3788c38a28e5b128ec25d86e117177e24e`
- 三个刀模测试入口通过：模型 10 组，图/原生端口/恢复/生命周期 13 组，成对事务 6 组。
- 新测试覆盖独立世界/HUD 恢复、实体销毁/复用、图重建、外国模型/参数不覆写、许可中途失效、不可读恢复失败锁存、失败后 epoch 丢弃、不重复原生调用、候选超时、禁用且不再收到旧刀内部回调、同一边界不重复加载。
- 128 次稳定维护断言模型路径读取计数不增长，且模型/重收集调用数不增长。既有 484 有向刀型组合和十万次轮询测试继续通过；均为模拟，不是 484 组现场实测。
- 与本机安装 PE 的所有指令证据对照及逐字节变更/截断拒绝通过；9 个枪皮入口通过；完整 **48 项**通过，**119.140 秒**。
- EN / ZH / Tests / 只读 Probe 四个目标编译成功。现有第三方及旧 Runtime 编译警告仍有，未新增编译错误。
- 六项隔离加载通过；无窗口 WARP、内嵌 DXBC、ImGui 像素读回及三次重建通过。
- PE 静态依赖仅 Windows 系统 DLL（d3d11/user32/shell32/version/kernel32/imm32），未增加 VC 可再发行运行库依赖；中英文 UI 和中文皮肤文本验证通过。隔离进程测试不是纯净虚拟机或朋友电脑验证。

验证报告：`build/knife-lifecycle-dev/validation/report.json`、`portable-load.json`、`VALIDATION.md`。

## 下一步：只做真实模型/动作验收，不再重复只读探针试验

1. 用 HLAE 的 `-insecure` 离线 Demo 新进程加载本开发版；不要往已加载其它 nSkinz 版本的进程重复注入。由用户操作 nSkinz 面板。
2. 同一玩家先确认源刀型，再选“饰面＋模型”目标 M9；记录 `boundaries`、`nested_updates`、`attempts`、`applied`、`faults` 及两个实际模型定义。源 Skeleton 525 → M9 508 是前阶段现场绑定已读到的组合，不是本版已经成功换模的结论。
3. 画面验收拔刀、攻击、完整检视、收刀再持刀；核对目标几何、握持位置、手臂/刀身穿模和动画。尤其检查首次强制更新的 call1 分支和后续 call2 分支，不能仅以材质变化判断刀型成功。
4. 反复切枪、换玩家、修改目标刀型、切“仅饰面”、禁用、拖动进度、重新播放/切 Demo。同步确认世界刀与 HUD 不残留，恢复失败不会反复写入。
5. 在同一场景对比原版与开发版帧时间；结合回调耗时检查资源加载和图验证成本。不能把模拟维护计数当作“卡顿已解决”。
6. 同时更改刀饰面、枪皮和手套，检查新链没有覆写已验证的饰面结果；模型路径变更后材质缓存是否需要独立刷新仍需现场结果。
7. 若任何模型、图或动画失配，先保存两份日志及配置/对应帧，再分析。不要放宽归属或指令证据门槛，也不要恢复旧的全局 Query/模型写入链。
