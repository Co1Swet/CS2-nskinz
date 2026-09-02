# 刀模续作检查点：真实模型资源与图骨架引用

> 最新续作：[原生端口实现与生命周期接线缺口](knife-model-checkpoint-20260831-native-port.md)。用户已加载本资源版并取得有效原生绑定/1920 次更新检查；不要再按下文历史状态假设尚未加载。

日期：2026-08-31。接续 [事务暂停点](knife-model-checkpoint-20260831-transaction.md)。v0.4.4 发布完成后继续本地独立刀模开发，未改变该 Release 或发布新的版本。

## 当前状态

本轮已补齐独立刀模事务所需的资源读取与验证：从实际 world/HUD/arms 模型句柄读取加载的数据，检查刀模型携带的骨架引用是否进入同一 arms 动画图。新增代码只读，不加载资源、不改引用计数、不修改模型/动画/涂装。**Windows `KnifePairAccess` 写入端口仍未接通，不是可用的换刀模版本。**

目前没有 CS2 进程，未启动游戏、注入 DLL、接管桌面或采集截图。上轮 probe2 未加载的现场缺口仍存在；本轮新开发 DLL 包含 probe2 的只读发现修正和新增资源检查，不能把自动化结果当作现场回调通过。

## 新确认的原生证据

CS2 Client/Server 2000899 / Patch 1.41.7.8 / Steam Build 25000182，Source revision 10948930。本机 Schema 文件再次核对：`CSkeletonInstance::m_modelState=0x140`、`CModelState::m_hModel=0xA0`、`m_ModelName=0xA8`。句柄字段为 scene +0x1E0。

| client.dll RVA | 实际含义 |
| --- | --- |
| `0x21CFC1` | 模型 getter 从 skeleton +0x1E0 取 binding，再解引用 binding 的首指针取得加载的模型数据 |
| `0x1655A80` | 模型 +0x430 的 Nm 骨架引用数量 |
| `0x16559E0` | 三参数输出式 getter，从模型 +0x438 取数组；元素步长 0x50，返回元素首个 8 字节引用 |
| `0x12628AF` | 图的副骨架引用向量位于 graph +0x338，count 在首字段 |
| `0x12628DE` | 引用向量 data 在 +8（graph +0x340），复制步长 8 |

这五个精确指令窗已加入生产验证器；真实磁盘 PE 检查通过。每个窗口逐字节破坏及截断都被测试拒绝。不能把模型资源 binding、加载的模型数据、骨架引用、动画图四种指针混为一谈，也不能把模型引用数组的 0x50 步长套到图的 8 字节引用列表。

## 实现

- `src/Runtime/KnifeResources.hpp/.cpp`：无 Write/Call 接口的独立解析器。两次完整采样并再次核对图 owner/instance；拒绝无效绑定、越界/溢出、空或重复引用、资源读中变化、arms 与刀身模型别名。支持 world/HUD 合法共享同一加载模型。
- 22 种当前支持的刀模型已由上一阶段 VPK 分析证明各有一个 Nm 骨架引用；本轮解析器要求该数量为 1，遇到不同布局应重审而不是猜测。
- `blade_skeletons_match` 与 `hud_skeleton_in_graph` 独立报告。部分提交或重收集尚未完成时，可以读出稳定快照，但不能将其标记为事务就绪。
- `FindKnifeModelAssetByPath` 用真实模型路径识别刀型，支持 `.vmdl` / `.vmdl_c`；不用 Econ definition 冒充模型变更成功，也不把 animation class 当模型路径。
- `Runtime.cpp::ReadIndependentKnifeResourcePair` 额外验证 Schema、scene owner、四个实体的完整身份/创建代际、模型名称前后稳定和 scene epoch。读取发生在原有低频独立探针，非每帧模型刷新。
- 新日志 `Independent knife resource pair` 输出源模型定义、加载模型、骨架是否相同、是否进入图及图引用数量；仅资源/绑定状态改变时输出，始终注明 `model_writes=0, animation_writes=0`。
- `tools/KnifeLiveProbe.cpp` 同时链接生产资源解析器，供后台 `PROCESS_QUERY_INFORMATION | PROCESS_VM_READ` 验证；辅助程序无写进程权限，无注入功能。未用 Python 复刻解析器替代生产 C++ 验证。

## 验证与产物

- 中英 DLL、测试程序和外部只读检查器全部构建成功。
- 刀模定向：3 个测试入口，其中图验证 10 组，模型控制器 10 组，成对事务 6 组。新增资源检查包含 17 类损坏/边界输入、部分提交、共享模型和采样漂移。
- 9 个枪皮检查通过，包括 v0.4.4 原皮 AWP 初始化；完整 48 项回归通过（129.015 秒）。模拟事务的 484 种组合与 10 万次维护回调仍不是游戏动画或 FPS 实测。
- 两语言共六项隔离加载、无窗口 WARP/ImGui 像素读回与三次资源重建、PE 导入及中文界面/目录文字检查通过；不是纯净虚拟机测试。
- 新产物：`build/knife-resources-dev/`。详细报告、DLL 哈希见该目录 `VALIDATION.md`。旧的 `knife-transaction-dev` / `probe2` 和 v0.4.4 文件均未覆盖。
- 源码快照：`build/checkpoints/knife-resources-20260831/source-checkpoint.zip`，文件校验清单位于同目录 `manifest.json`；包含源码、工具、开发文档和项目文件，不包含未变化的第三方组件（可使用 v0.4.4 基线的 `third_party`）。

## 下一步：现场闭合后再接通写入

1. 用户通过 HLAE `-insecure` 自行打开离线 Demo。必须重启加载过其它 nSkinz DLL 的 CS2，只加载本轮开发 DLL，不叠加版本。
2. 在 nSkinz 面板给当前观战玩家选择刀的“饰面＋模型”，目标选择不同刀型，然后更新；播放、持刀、切到其它武器再切回和检视由用户操作。
3. 后台读取 `build/knife-resources-dev/zh-CN/nSkinzCS2.log`。先要求 `native_binding_verified=1`、`source_pair_ready=1`，再核对 `Independent knife update timing` 两个调用点/线程/源参数计数。若 `legacy_observer_verified=0`，继续查该门槛与实际观战视角的对应关系，不直接删除写入授权校验。
4. 正确回调时序仍需现场证据：原生 weapon_type 更新返回之后、图求值之前；计数是进程生命周期累计值，不是每个玩家独立通过证明。
5. Windows 原生端口需把新解析器接入 `Read`，并按已确认的 SetModel 包装器 → 显式副骨架重收集 → 条件 ID 参数更新顺序实现；真正调用前后都验证绑定。不得把“原生函数返回”当成资源和附件就绪。
6. 完善退休时机：收刀、换玩家、禁用可能不再收到旧刀的更新回调，不能丢弃捕获状态后遗留世界模型，也不能在未知时机回写旧 HUD。seek/图重建放弃失效地址；目标/源 symbol 的允许范围仅限本事务捕获的 binding。
7. 接通后再实测实际换刀、切刀、攻击、检视/特殊检视、穿模和帧时间，并回归枪皮、刀饰面、手套的独立性。当前开发 DLL 没有完成这些画面验证。
