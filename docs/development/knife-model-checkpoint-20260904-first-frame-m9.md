# 刀模首帧接管与 M9 HUD 崩溃检查点（2026-09-04）

## 范围

本检查点继续基于 v0.4.5 源码开发，不是新 Release。枪械涂装、手套、挂件和刀的 `Finish only` 链没有并入刀模事务；本轮没有启动、注入或操控 CS2。

## 0.4.5 现场日志结论

朋友提供的 `nSkinzCS2 (2).log` 与 `nSkinzCS2.ui (2).log` 显示：

- 目标由蝴蝶刀 515 改为 M9 刺刀 508 后，源刀为骷髅匕首 525。
- 运行时先读到完整的 525 world/HUD/graph，随后将 world 和新 HUD 都变为 508。
- 最后一条事务统计是 `attempts=8, applied=7, world_prepared=8, hud_handoffs=7, faults=0, phase=7`，即第八次世界模型提交已完成，但 M9 HUD 尚未被事务接纳。
- 日志在目标 HUD 已出现后直接中断，没有进入普通失败/回退记录。UI 日志也在下一秒停止。

这将崩溃范围收窄到旧版“活跃世界刀 SetModel → 删除/重建 HUD → 下一回调重收集副骨骼”的窗口，而不是输入框、普通枪皮或全局材质生成。旧链每次收刀后恢复、下次切刀再重建，也解释了无部署动作、约两秒后刀突然出现的问题。

## 当前修复

1. 收起状态预装：在刀未激活时预先写入目标 item definition、目标世界刀模型和饰面属性，保留精确的源状态恢复账本。
2. 提前监听：预装成功时立即安装并监听原生 arms/weapon 更新，不再等待活跃刀被 75 ms 轮询发现。
3. 首个 `AfterWeapon` 原子接管：在外层 arms 图继续求值前，按 scene epoch、XUID、pawn/world/arms/HUD 完整句柄、creation tick、持有者、HUD 场景树、graph controller 和 graph instance 重新构造绑定。
4. 预装事务采用：确认 world/item 已是目标后不再次写它们，只将已有写入转入事务恢复账本；随后仅在需要时处理 HUD、目标副骨骼和 `weapon_type`。
5. M9 重收集保护：如果 Source 2 已原生创建目标 HUD 且副骨骼已经匹配，跳过多余的 Recollect。只有 HUD 模型确实改变或骨骼读回不匹配时才调用一次。
6. 旧模型残留归一：恢复目标以源 item definition 对应的规范模型为准，不再把“物品定义与模型路径不一致”的旧残留路径保存成新的恢复目标。
7. 禁止持刀中途重建：新请求到达时如果源刀仍处于活跃状态且没有精确预装租约，只进入等待；切回枪后先在收刀状态预装，下一次切刀再接管。生产路径不再用 0.4.5 的活跃 world SetModel 强行追赶请求。
8. 观察者库存缺失恢复：2026-09-04 首轮实测日志显示 `59 → 507` 始终停在 `waiting_holstered_prearm=1`，且在默认 T 刀与当前手枪之间交替，证明该 Demo 的观察者快照在切枪后没有稳定复制刀的 `m_hMyWeapons` 项。现在持刀时先只读保存完整 world 身份；切枪后不依赖库存数组，而是重新核对同一 world 的完整 handle/serial、creation tick、实体地址、item view、pawn owner 和实时 active handle，确认它确已收起后才预装。句柄复用、所有权转移或任一身份变化都会丢弃观察票据，不会写旧指针或别人的 `.drop` 刀。
9. 所有写入仍要求 HLAE、`-insecure`、离线 Demo、正确游戏线程和原生回调动态作用域；任何身份或资源证据不一致均只读退出。

## 关键提交顺序

预装成功后，第一次切刀的目标顺序为：

`目标 world/item/finish 已就绪（收刀时） → 原生创建首个 HUD → AfterWeapon 验证 → 必要时目标 HUD SetModel → 必要时 Recollect → weapon_type 切到目标 → 继续原生 arms 图求值`

如果原生 HUD、目标副骨骼和 `weapon_type` 已全部正确，接管不会重复 SetModel 或 Recollect。稳定状态只维护被原生 VData 重写的 item definition / `weapon_type`，不会每帧重载模型。

## 后台验证

- 当前 `client.dll` / `animationsystem.dll` 的已知指令窗校验通过。
- 刀图测试：3 个入口、16 组图/生命周期测试通过。
- 成对刀模测试：22×22 共 484 个有向组合，7 组事务测试通过。
- 新增目标 HUD 已原生继承的 M9 用例：world、HUD 和副骨骼已经为 508 时，提交只允许 `weapon_type` 条件切换；world SetModel、HUD SetModel、Recollect 均为 0 次。
- 本次修复重新编译后，刀模、刀图、兼容性、配置、武器/手套/挂件和材质链完整原生回归全部通过。
- 中英文 x64 `/MT` 开发 DLL 均通过 6 项隔离 LoadLibrary、System32-only PATH、冲突编译器环境和无窗口 WARP/内嵌 DXBC 验证。

开发 DLL：

- `build/knife-v045-holster-observation-dev/en/Lyan_nskinz v0.4.5 EN.dll`
  - SHA-256: `aa9d1f727034414e14cea161be3631373a1fc8e29e2053b8c48dcbd28973becf`
- `build/knife-v045-holster-observation-dev/zh-CN/Lyan_nskinz v0.4.5 zh-CN.dll`
  - SHA-256: `53339b07d175ce214bdcb088d400501b88ab67a40dbc5291d3e0845d65aac5b1`

两者直接导入仅为 Windows 系统组件：`d3d11.dll`、`user32.dll`、`shell32.dll`、`version.dll`、`kernel32.dll`、`imm32.dll`。

## 尚需实际 Demo 验收

后台测试能证明事务顺序、拒绝条件、ABI 指令窗和便携加载，不能证明视觉帧。下一轮实际 Demo 需要重点验证：

1. 骷髅匕首 525 → M9 508 第一次切刀，无崩溃、无两秒延迟、部署动作连续。
2. 默认 CT 42 / 默认 T 59 → M9、蝴蝶、爪子、骷髅，第一次切刀即使用目标动作。
3. 已原生继承目标 HUD 时日志出现 `prearm_callback_stages=1`、`prearmed_adoptions=1`，且 `prearm_callback_rejects=0`。
4. 重复切枪/切刀、检视、暂停、拖动 Demo、切换玩家、`.drop` 后，不出现旧刀实体残留、穿模或错误 HUD 图标。
5. 同时启用枪皮、手套和刀涂装，确认独立链没有回归。

只有这些视觉和生命周期实测通过后，才能把本检查点作为后续 Release 的刀模修复结论。

## 20:02 实测回退与手套空槽补丁

`knife-v045-holster-observation-dev` 的 20:02 实测不是旧 DLL：日志出现了
`Remembered authoritative active knife` 和新增预装统计字段。它稳定读到源定义
`59`、目标 `507`、完整 world/HUD/arms/graph，但 `model_attempts` 始终为 0。
原因是收刀预装还要求旧 `C_CS2HudModelWeapon` 对象必须销毁；该 Demo 会缓存隐藏
HUD 对象并保留 owner edge，因此合法的收刀状态被永久拒绝。当前补丁继续用 Pawn
实时 `active_weapon != remembered_world_handle` 禁止持刀中途写入，同时不再把隐藏 HUD
对象仍分配视作活跃部署。预装只写已重新验证的 world/item/finish，下一次原生 equip
回调再接管 HUD、骨骼和 `weapon_type`。

朋友的 `(3)` 日志是独立的手套问题。既往成功实测中的 Pawn `m_EconGloves`
定义均已是 `5028/5029/5030/5032/5034`；新日志的同一字段可读但为 0，旧门槛把它
错误导向无法安全追加空 `m_hMyWearables` 的 CEconWearable 创建后备。当前补丁只允许
`model_and_finish + 可读空槽 + 已知合法目标手套定义` 从 definition 0 初始化；
`finish_only` 空槽、未知目标和不可读槽仍然拒绝，且没有恢复任何裸容器扩容写入。

新开发 DLL 使用独立名称以避免与 v0.4.5 Release 混淆：

- `build/knife-glove-prearm-fix-dev/en/Lyan_nskinz v0.4.5 knife-glove-fix EN.dll`
  - SHA-256: `9a682ebb6058cadfb9e4e8f6802cb4afb6f9fa0ebbb3a6277bd7c3c49f63bdcb`
- `build/knife-glove-prearm-fix-dev/zh-CN/Lyan_nskinz v0.4.5 knife-glove-fix zh-CN.dll`
  - SHA-256: `5ff6add308342cfc2985c9210dcc2c1dda1b3e35244c3bf07272132ff35c1524`

完整原生回归以及 6 项隔离加载、System32-only PATH、冲突编译器环境、WARP/内嵌
DXBC 检查通过。上述结果仍需实际 Demo 确认视觉与引擎重建行为。

## 2026-09-05 手套属性节点与刀预装精确失败诊断

20:21–20:22 的新开发 DLL 日志证明，手套已经正确命中 Pawn
`m_EconGloves`，并且已把 definition 从 0 写为 5032；实际失败是原
item view 不一定预置 paint/seed/wear 三个节点，而旧分支只能改
已有节点或覆盖已有容量。现在 Pawn 手套与已验证的原皮枪械使用
同一条安全链：通过 CS2 Schema 向量操作/原生 append 让引擎分配并构造
缺失节点，再在 `NetworkStateChanged` 通知后写值和回读；不再给
`m_EconGloves` 安装外部或借用的属性向量。

刀链同一份日志确认了 `59 -> 507`、完整 world 身份与收刀切换，
但在预装事务内部回滚，没有记录具体失败项。现在预装对 definition
写入、饰面节点构造/同步、econ 字段写入、回读、授权、实体代次、
owner、vtable 指令窗、原生 SetModel 及其后回读都有独立失败原因。下一次
实测会在日志中直接出现 `Holstered knife pre-arm failed: ... issue=...`，
不再使用笼统的“换刀无效”推测。

当前开发 DLL：

- `build/knife-glove-native-fix-dev/en/Lyan_nskinz v0.4.5 knife-glove-native-fix EN.dll`
  - SHA-256: `a628b619cc29bfe5e9157443f0556134b6936f6f982327fb955f87004d1ae8e7`
- `build/knife-glove-native-fix-dev/zh-CN/Lyan_nskinz v0.4.5 knife-glove-native-fix zh-CN.dll`
  - SHA-256: `02decee44d06abdf9a4ca0524d294f925fbd218cf54881eddf9aa9a0f6923d1e`

两个 DLL 均为 x64 `/MT`，直接导入仅包含 Windows 系统 DLL；全部本地
测试（包含刀模 11 组、刀图/生命周期 16 组、22×22 成对事务）通过。

## 2026-09-05 可见手套载体与刀预装快照修正

09:35 实测日志证明，手套 `m_EconGloves` 已成功回读 definition
`5032`、paint `10084`、seed `0`、wear `1e-07`，且重建标记已翻转；
但该 Demo 初始的 definition 为 0，Pawn item view 只是 econ 描述，没有
创建可见 `CEconWearable` 模型。因此旧逻辑把“字段回读+重建标记”
错判为画面已成功。现在 `finish_only` 仍使用 Pawn `m_EconGloves`，
`model_and_finish` 则优先等待已验证的实体工厂并绑定真实
`CEconWearable`；Pawn 只作工厂就绪前的短暂后备，不再阻断可见载体创建。

同一份日志已给出刀预装的唯一失败字段：所有 econ 快照字段可读，
仅收刀后 world entity 上的 render mesh mask 不可读。该 mask 在回滚代码中
本来就是可选项，却被错设为整个快照的必需条件，使 `59 -> 507`
在任何 definition/SetModel 写入前退出。现在快照仍要求所有会被写入的
econ 字段，mesh mask 则仅在成功捕获时才纳入回滚。

新开发 DLL：

- `build/knife-glove-visible-fix-dev/en/Lyan_nskinz v0.4.5 knife-glove-visible-fix EN.dll`
  - SHA-256: `e3a32d099051a48c4f09f076cba42be33357e945b994b80270af788b3a0110fd`
- `build/knife-glove-visible-fix-dev/zh-CN/Lyan_nskinz v0.4.5 knife-glove-visible-fix zh-CN.dll`
  - SHA-256: `03fe37a4f8c54e2ea180dcb07552c8535b381250fe41762b0779b1a4eba487fa`

完整本地测试再次通过；两个 DLL 仍为 x64 `/MT`，导入表仅包含
Windows 系统 DLL。此结论仅表示静态/模拟回归通过，真实 Demo 中的
`CEconWearable` 生成、刀预装与首帧动作仍需下一次画面实测确认。

### 09:56 双载体手套与 owner 刀实体恢复

`knife-glove-visible-fix-dev` 的新日志证明：已通过引擎工厂创建
entity `2013`，已将 handle `0x87DD` 追加到 `m_hMyWearables`，definition
`5032`/paint `10084` 回读正确，Wearable `SetModel` 也已验证成功；
但 Pawn `m_EconGloves` 仍是 definition 0，因此第一人称手臂没有消费
这个 Wearable。现在 model+finish 会以引擎自有节点同时更新真实
Wearable 和 Pawn econ item view，然后触发 Pawn 手套重建。

刀日志在 Update 后能验证活动刀 `59`、HUD/arms/graph 和 owner，但
切到枪时 Demo 发生了 Pawn 代次重建，旧 active observation 被正确废弃，
新 Pawn 的 `m_hMyWeapons` 又没有列出收起的刀，因此预装无入口。
现在只在没有可用 observation/lease 时执行一次恢复扫描：要求刀实体
的 owner handle 严格等于当前 Pawn、实体处于收刀状态且候选唯一。
成功后转为 O(1) lease 维护；多刀歧义时仍只读拒绝，不猜测实体。

当前开发 DLL：

- `build/knife-glove-dual-carrier-fix-dev/en/Lyan_nskinz v0.4.5 knife-glove-dual-carrier-fix EN.dll`
  - SHA-256: `72ba0a2372da4af4054a8ea9d04f9ba19465c807b76f1a648426d3cb7e10e9f8`
- `build/knife-glove-dual-carrier-fix-dev/zh-CN/Lyan_nskinz v0.4.5 knife-glove-dual-carrier-fix zh-CN.dll`
  - SHA-256: `c7248843613a40b578e6633fcaffcf7375e6db4f8bd0904766841f878edf9880`

两个 DLL 仍为 x64 `/MT`，仅导入 Windows 系统 DLL；完整原生测试通过。

### 10:14 崩溃回退与单一权威载体修正

`knife-glove-dual-carrier-fix-dev` 的实测日志证明刀实体 817 已完成
`59 -> 507` 和 paint 568 的收刀预绑定，但画面未生效后进程退出。同一窗口内，
旧 ResourceSystem 查询交换器进入超过 1000 次却始终 `Hits=0 / Pairs=0`，
最大渲染回调耗时 708 ms；临时 `CEconWearable` 则在手套双载体路径中被重建并
重复进入引擎 definition-refresh/SetModel。当时未找到 Application Error 事件或
dump，但转储搜索范围不完整。下方 14:12 转储分析已撤回“没有 dump”和据此
归因于查询钩子/双手套载体的结论；这两项调整不是已证实的崩溃根因修复。

当前修正：

- 退役的全局刀资源查询交换器已 fail-closed。当前生产刀链只使用 owner /
  entity-generation 验证的独立事务；手套不再误启用刀查询钩子。
- 手套恢复 Pawn `m_EconGloves` 为唯一权威源，`finish_only` 与
  `model_and_finish` 均优先通过该引擎所有的 item view 应用；只在 Pawn 槽不可读时
  才使用既有 Wearable 后备。不再为同一观战 Pawn 同时刷新两个手套载体。
- definition 0 的空手套槽仍仅在 `model_and_finish + 已知合法目标定义`
  时初始化，并使用引擎所有属性节点构造；没有恢复外部容器或裸向量扩容。

新的本地开发 DLL：

- `build/knife-glove-canonical-fix-dev/en/Lyan_nskinz v0.4.5 canonical-fix EN.dll`
  - SHA-256: `8448df4c06d3683fc54fbf24ec23f8ff672b02f8b3e596afe50e738a9dbf2c50`
- `build/knife-glove-canonical-fix-dev/zh-CN/Lyan_nskinz v0.4.5 canonical-fix zh-CN.dll`
  - SHA-256: `23f80ade4c06e8213575b41c64e469896beefad51ae154598601bea900ee19d8`

完整原生回归、刀模 11 组、刀图/生命周期 16 组、22×22 成对事务、6 种
隔离加载与无窗口 WARP/DXBC 检查已通过。这些结果不替代 Demo 画面验收；
实测前必须完全重启 HLAE/CS2，不能在仍驻留旧查询钩子的进程内叠加新 DLL。

### 14:12 再次崩溃：已定位 AnimAssetData 缺失的目标刀骨架

`canonical-fix` 仍在 14:12:28 崩溃。现已找到并用实际 PE unwind metadata
展开以下两份转储，而非把栈里的任意模块地址当作调用帧：

- `game/bin/win64/cs2_2026_0905_101427_0_accessviolation.mdmp`
- `game/bin/win64/cs2_2026_0905_141228_0_accessviolation.mdmp`
- Steam 同时在 `D:/steam/dumps/` 保存了对应 `crash_cs2.exe_...dmp`。

两次都是 `0xC0000005`，`client.dll+0x34FC53` 执行
`mov rsi,[rdi+8]`，其中 `rdi=0`。共同调用链为
`346ECF -> 8FE462 -> 34F709 -> 34FC53`；上午在动画工作线程，下午在
FrameStageNotify 原生回调中的动画任务。下午的 pawn 地址与手套日志相同，
但不能据此归因为手套：两份转储的子调用栈残留均明确包含
`knife_karambit.vnmskel`。

实际链路：`client+3687D0` 从资源 descriptor 取名字，通过
`client+2239700` 的 `AnimAssetData` 表 `FindString`（vtable +0x58）查行，
返回 -1 时不产生输出。`client+34FC53` 随后无条件解引用这个空输出。
`59 -> 507` 换模预装已把目标骨架引入 pawn 的动画上下文，但 Demo 没有
录制该骨架对应的数据。此前只验证 vmdl / vnmskel 已加载，遗漏了该依赖。
注意 definition `507` 是爪子刀；M9 是 `508`，不能把这份现场称作 M9 实测。

新增 `Runtime/KnifeSerialization.*`：

- 在独立刀链调用 SetModel **之前**装入已校验原生 ABI 的动画上下文适配。
- 仅在离线 Demo、指定原生调用点、已被本 DLL 改模的已知刀骨架，且该
  骨架在 `AnimAssetData` 缺行时，调用游戏已有的本地动画上下文构建函数
  `client+1266670`。六参数缓存入口到五参数本地入口的映射来自
  `8FE43A..8FE480` 相邻原生分支，保留所有 graph/animation-set/skeleton 输入。
- 不伪造空缓存，不吞掉原生构建异常，不修改 `ag2_use_networked_serialization_context_demo`，
  不关闭动画，不写枪械/手套/涂装字段，不恢复全局 ResourceSystem 查询交换器。
- 有完整录制数据、非刀骨架、未被改模的刀或非 Demo 均继续原有路径。
- 动画线程不写日志、不扫描实体、不分配工具侧容器；命中计数由工作线程输出。
- 适配代码固定在 CS2 进程生命周期内，因为异步任务可能在关闭面板后继续
  持有目标骨架。只持有稳定引擎接口/函数地址和原子状态，不持有 UI/AppState/
  entity/graph 指针；更换测试 DLL 必须重启 CS2。

新转储诊断工具 `tools/parse_minidump.py --inspect ... --unwind` 只读本地 dump，
unwind 要求本地模块时间戳及 SizeOfImage 匹配。没有附加游戏或控制桌面。
匹配旧转储的 PDB 已保存在
`build/checkpoints/crash-20260905-animassetdata/canonical-fix-zh-CN.pdb`。

本轮测试版：`build/knife-animasset-fix-dev/`。验证状态和文件哈希见同目录
`VALIDATION.md` / `portable-load-report.json`。完整原生测试、484 个动画路由组合、
参数转发与当前安装游戏的实际指令窗已通过；这些是后台离线验证，**不是
Demo 画面验收或“全部闪退已消除”的证明**。手套可见效果、首刀动作、拾取/
切枪/seek 和连续播放仍需同一故障 Demo 实测。
