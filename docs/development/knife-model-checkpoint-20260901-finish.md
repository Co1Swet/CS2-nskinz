# 刀模暂停点：目标刀饰面接续与 CT/T 默认刀（2026-09-01）

接续 `knife-model-checkpoint-20260901-hud-handoff.md`。用户已确认上一版换模成功，但 M9 是原皮，没有选定的多普勒。本轮仅制作本地开发版；未控制桌面、未向游戏注入新 DLL、未发布或修改 v0.4.4。

## 已确认与待确认

- **用户确认**：上一版两阶段世界刀 / 新 HUD 换模成功。保留该实现，不再把饰面配置纳入刀模事务身份。
- 现场配置为 NiKo、源 Skeleton 525、目标 M9 508、paint 568。旧日志在换模前执行饰面初始化，之后命中源 definition 的饰面快速路径；新 HUD 换模完成后没有目标模型的独立材质生成。
- 原日志和配置复制至 `build/knife-model-finish-dev/evidence/`，仅本地保存。
- **本轮未实测画面**：新的“模型＋饰面”、目标刀原皮、CT/T 默认刀、连续切刀帧时间、完整动作与穿模仍需用户用新 DLL 验收。单元测试、材料数量和成功返回值都不等同于画面成功。

## 本轮实现

1. `BuildKnifeAppearanceRequest` 只在有效的“模型＋饰面”且 paint=0 时派生原皮意图。保存的“-”不被改写；仅饰面中的“-”仍是不改皮肤，缺失或无效模型不自动删除原饰面。
2. 有效独立刀模请求绕过旧源刀饰面刷新，等到目标世界 / HUD 模型、副骨架、归属都核验后，执行新的 `PollIndependentKnifeFinish`。原生同刀型无换模的情况也可接入饰面；默认 CT 42、T 59 可作为源刀。
3. `KnifeFinishLatch` 以完整 world/HUD/pawn/arms 身份、scene epoch、目标刀型、成功换模代数及饰面签名为键。新 HUD 或新代数不会复用源刀饰面缓存；同代数同饰面仅执行一次重型刷新。失败锁存，不逐帧重试。稳定期 250ms 只校正饰面属性，不重建材质或模型。
4. 默认 CT/T 刀缺少 paint/seed/wear 属性时，复用已验证的 Schema 原生节点构造和逐项写入，最多添加缺少的 6/7/8，不置换属性容器，不复制 vtable，不改注册哈希元数据。源 Econ definition 保持 42/59/原刀型；材质缓存键使用目标刀型。
5. 独立饰面适配器只调用已验证的当前刀具材质清理、目标资源生成及原生 world → HUD 材质传递。正涂装要求两端均读回主材质；原皮要求两端材质清空，并通过原生移动赋值/析构释放旧生成参数，使用目标模型默认材质组。
6. 不调用全局 `RegenerateWeaponSkins`、SetModel、Recollect，不递增 custom econ reload event，不改共享 VData、arms 或动画符号。材料前后检查模型、arms、骨架和动画身份不变。原皮参数函数的完整扫描按 client 模块缓存，避免每次拔刀重复扫描。
7. 面板中英文说明更新：已选刀型时，“-”为目标原皮；选定涂装时同时换模型和涂装；CT/T 默认刀也可换其它刀型。

## 安装文件中的原生证据

基线仍为 CS2 Client **2000899** / Patch **1.41.7.8** / Steam Build **25000182** / source revision **10948930**，HLAE **2.191.1**。

- `client+7D97F0` 是原生单个世界武器材质生成器，`1439FF0` 是 owner 清理入口。仍由已验证全局 helper 调用窗口解码，但不执行全局 helper。
- `7D9EA1..7D9EC1` 把世界刀 `+1848` 参数交给 `+608` 材质 owner 的 virtual+8。
- `92BB60` 的 virtual+8 在参数没有模型资源时调用 `21CF00`；`21CF53` 读取当前 skeleton/model state 的 `+1E0` 模型资源。因此无需把源刀物品定义永久改成 M9。
- 世界 `C_Knife` 和 `C_CS2HudModelWeapon` 的 `+608` owner 虚表均使用 `92BB60` / 材质变化回调 `925960`；运行时逐对象验证槽位。
- `104F314..104F32E` 的原生 HUD 构造路径调用 `143A130(source_owner, hud_owner, false)`。该函数通过 `143A2F0` 使用引擎引用计数传递材料，而非复制裸向量。新适配器使用相同入口；绝不把 HUD 当成世界武器调用 `7D97F0`。
- `KnifeFinish.hpp` 收录五个完整指令窗口；安装文件逐字节校验、每字节扰动拒绝和截断拒绝均加入后台测试。

## 验证与产物

- 开发版：`build/knife-model-finish-dev/en/nSkinzCS2.dll`、`zh-CN/nSkinzCS2.zh-CN.dll`。
- 新增 CT/T → M9 / 蝴蝶刀 / 骷髅匕首 × HUD 继承或不继承目标的 12 个生命周期场景，包含关闭后恢复正确源刀型。
- 新增 CT/T 空属性初始化、多普勒属性切换、种子/磨损、原皮清除、恢复；1000 次稳定属性操作不重复追加节点。
- 新增同饰面一万次不刷新、失败一万次不重试、新 HUD / 代数 / 玩家 / 目标重新初始化的预算测试。
- 刀模 3 个测试入口（模型 11 组、图与生命周期 15 组、成对事务 6 组）、枪皮 9 个入口以及完整 48 项回归；以 `validation/report.json` 为最终结果和测试二进制哈希依据。
- 六项中英文隔离加载、无窗口 WARP/DXBC/ImGui 像素读回；Windows 系统导入和中文文本检查。没有新增第三方运行库依赖；不是纯净虚拟机或朋友机器测试。
- 源码与产物哈希、验证结果：`build/checkpoints/knife-model-finish-20260901/manifest.json`；源码包 `source-checkpoint.zip`。旧成功换模版和公开 Release 保持原样。

## 下一次实测

退出旧 CS2，用 HLAE `-insecure` 新进程加载开发 DLL；不要在已经加载旧 nSkinz 的进程中重复注入。

```text
mirv_loadlibrary "E:\codex项目\CS2-nskinz-main\build\knife-model-finish-dev\zh-CN\nSkinzCS2.zh-CN.dll"
```

用 DLL 面板分别测试：

1. 当前已成功的 Skeleton → M9，选择多普勒；再改伽玛多普勒、种子和磨损。
2. 同目标将涂装改“-”：应为 M9 原皮，不能继承原刀涂装。
3. CT 默认刀 42、T 默认刀 59 → M9 或其它目标；分别测试“-”与正涂装。
4. 连续切枪/切刀、检视与攻击、换玩家、seek、禁用、仅饰面，以及同时替换枪皮和手套。

诊断日志新增 `Independent knife target finish`：source/target/paint、HUD handle、model_generation、finish_attempts、native_verified 及具体失败原因。`native_verified=1` 只证明内部材质状态，必须另看实际画面和帧时间。
