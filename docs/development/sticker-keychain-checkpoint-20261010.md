# 贴纸与挂件功能检查点（2026-10-10）

本文记录 `v0.6.0-beta` 之后贴纸功能的实际完成范围，并确定枪械挂件增加／替换的实现边界。后续开发和回归测试以本文为检查点，不再把“挂件属性存在”误认为“挂件模块已经创建”。

> **2026-10-26 更正：** 第 3–6 节对“Demo 中存在独立挂件网络实体，以及新增必须注入该实体”的假设未经验证，已被四份真实 Demo 的类表核查纠正。客户端模块存在不代表同名网络实体存在。后续挂件路径与证据以 [挂件重建核查](keychain-rebuild-audit-20261026.md) 为准；本页相关部分仅作为历史方案保留。

## 1. 当前贴纸进度

已经完成：

- 枪械五个逻辑贴纸槽位均可配置。
- 已有贴纸可原位替换图案，只改 kit ID，并保留源 Demo 中的磨损、缩放、旋转和自由摆放数据。
- 源 Demo 不存在的贴纸槽可由“临时换肤 Demo”补建；重写器会扩展 Econ 动态属性向量，而不是依赖固定的原向量长度。
- 自定义位置可写入磨损、缩放、旋转、offset X／Y 和 schema。当前使用的字段族是：
  - legacy 贴纸属性：`113–132`；
  - CS2 位置增量：`278–287`；
  - 位置 schema：`290–294`。
- offset X／Y 按“武器材质预设锚点 + 每槽位增量”处理；编辑器保存的是增量，不会把材质锚点重复叠加。
- 预览画布显示具体枪型、枪口朝左的轮廓以及五个槽位的相对位置。选中槽位为黄色，其余已配置槽位为白色。
- 可选游戏内实时预览会以节流方式应用编辑结果；最终画面仍以 CS2 的模型曲面、视角和材质代理为准。
- 临时 Demo 写出后会重新解析，对每个请求槽位和每个请求字段执行精确 32 位验证；验证失败时不会自动播放生成文件。
- 原 Demo 从不覆盖，所有变化只写入新的临时 Demo。

当前边界：

- 画布是枪械贴纸空间的二维投影，不是 CS2 的完整三维表面、遮挡和曲率渲染器。
- 运行时直接修改适合已有槽位；缺失槽位的可靠创建仍以临时 Demo 重建为主。
- 每次 CS2 更新后仍需重新验证材质中的五个预设锚点和 Schema 字段。

## 2. 当前挂件进度

已经具备：

- 从本地 `items_game` 的 `keychain_definitions` 读取挂件 ID、名称和模型资源路径。
- 按 `XUID + 枪械 definition` 保存挂件配置，支持选择挂件和明确清除。
- 已识别当前 slot 0 Econ 属性块：
  - `299`：挂件 ID；
  - `300/301/302`：offset X／Y／Z；
  - `306`：seed；
  - `314`：highlight；
  - `321`：sticker／附加外观字段。
- 对源 Demo 已经存在的 `C_KeychainModule`，运行时可以写入 `m_nKeychainDefID`、`m_nKeychainSeed`，刷新模型，并执行读回验证。
- 已有模块可以替换或清除；实体查找会核对武器父子／拥有者关系，而不是只按相邻实体索引猜测。
- 代码中已有通过当前客户端类工厂创建本地 `C_KeychainModule`、绑定父武器和跟踪清理的基础设施。

尚未完成：

- “临时换肤 Demo”的请求协议和 Rust 重写器尚未接收 `keychain_profiles`。
- Demo 中已有挂件模块的字段还没有在重写阶段持久化替换。
- Demo 中完全没有挂件模块时，当前 Demo 控制器仍明确工作在 existing-module-only 模式，不会创建缺失模块。
- 挂件位置／seed／highlight 尚未开放为完整编辑项；当前新增请求应先使用枪械默认挂点和默认值。

## 3. 为什么不能把挂件完全照搬贴纸实现

贴纸的最终状态位于枪械 ItemView 的 Econ 动态属性向量中。只要在武器 `Created` 数据里补齐对应属性，CS2 就能让同一武器实体的材质代理读取它们，不需要再创建一个网络实体。

挂件除了武器 ItemView 中的 `299/300/301/302/306/314/321` 属性块，还存在独立的 `C_KeychainModule` 实体。这个实体拥有自己的 definition、seed、模型状态，并通过 owner／move-parent 等实体句柄与枪械关联。因此：

- 只替换已有挂件：可以沿用 Demo 重写思路；
- 给原本没有挂件的枪新增挂件：只扩展武器 Econ 属性并不足以证明模块已创建；还必须得到一个生命周期正确、父武器正确的 `C_KeychainModule`。

## 4. Demo 重建可行性结论

### 4.1 已有模块替换：可以走纯 Demo 重建

重写器应同时：

1. 在目标武器的 Econ 动态属性向量中覆盖或补齐 slot 0 的七个属性；
2. 找到归属于该武器的现有 `C_KeychainModule`；
3. 覆盖模块的 `m_nKeychainDefID` 和 `m_nKeychainSeed`，保留原 owner、parent、实体句柄和位置关系；
4. 写出后重新解析武器属性和模块字段，二者都一致才算成功。

这条路径不需要新增网络实体，风险与已有贴纸／涂装字段重写相近。

### 4.2 缺失模块增加：理论上可做纯 Demo，但当前写入器能力不够

纯 Demo 方案必须向 `CSVCMsg_PacketEntities` 注入完整的 `Created`／`Deleted` 生命周期，而不只是添加字段。至少需要正确处理：

- `C_KeychainModule` 的 class ID 和 flattened serializer；
- 未占用的 entity index、17 位 serial 和最终 handle；
- owner／move-parent 到目标枪械 handle 的绑定；
- definition、seed 和模型初始化字段；
- `updated_entries`、实体索引 delta、`serialized_entities` 长度表；
- 武器删除、掉落、重建、回合切换和 Demo seek 时的删除／重建顺序。

当前 vendored `source2-demo` writer 支持替换已有实体字段和为已有实体补字段，但没有公开的“插入新实体事件”接口。因此此阶段直接拼 bitstream 风险很高，容易再次产生卡加载、serialized entity length mismatch 或 seek 后悬空实体。

## 5. 推荐实现：Demo 重建 + 播放时模块创建

第一版新增挂件采用混合路径：

1. 将挂件配置并入临时 Demo 请求；重写目标武器的完整 slot 0 Econ 属性块。
2. 生成清单记录 `XUID + weapon definition + keychain kit + seed/offset`，用于播放时严格匹配。
3. 如果 Demo 本来已有 `C_KeychainModule`，优先在 Demo 内重写并继续使用该游戏拥有的模块。
4. 如果模块缺失，只允许在 nSkinz 生成的 Demo 中，由 DLL 等待武器、模型和拥有者连续稳定若干帧后，调用已经验证过的客户端类工厂创建本地 `C_KeychainModule`。
5. 把新模块绑定到目标武器 handle，写入 definition／seed，设置对应挂件模型，然后分别读回：
   - 武器 ItemView 的挂件属性；
   - 模块的 definition／seed；
   - 模块的父武器关系；
   - 实际模型资源。
6. Demo seek、武器实体 generation 改变、掉落或重新加载时，先解除并清理 nSkinz 创建的模块，再在稳定窗口重新创建。不得把本地模块当作服务器拥有的实体长期保留。
7. 任一验证失败都回滚或停用本次创建，不能循环创建重复模块。

这条路径仍然以 Demo 重建保存物品属性，但把“缺失独立实体的创建”放到已经存在的运行时实体工厂链中。它比立刻扩展 PacketEntities bitstream 更容易验证，也不会影响原 Demo。

## 6. 分阶段开发顺序

1. 将临时 Demo 请求升级到 schema v2，给每件枪增加可选 `keychain` 对象。
2. 扩展发现阶段：记录 weapon slot 0 属性、`C_KeychainModule`、模块 owner／parent 与枪械 handle 的对应关系。
3. 实现已有模块的纯 Demo 替换／清除，并增加写出后二次解析验证。
4. 在生成 Demo 清单中记录预期挂件状态。
5. 仅对“生成 Demo + 缺失模块 + 当前客户端工厂验证通过”的组合开启运行时模块创建。
6. 增加 seek、暂停、回合切换、掉落／拾取、观察目标切换和多把同型号枪的回归测试。
7. 最后再评估是否给 `source2-demo` writer 增加通用 synthetic entity API，以实现不依赖 DLL 的纯 Demo 挂件创建。

## 7. 验收标准

- 已有挂件可替换、清除，重播和 seek 后结果一致。
- 无挂件枪可以新增一个挂件，不重复、不串到同型号的另一把枪。
- 第一人称、第三人称、掉落武器和观察者视角的父子关系一致。
- 更换挂件不会改变枪皮、贴纸、名称标签或 StatTrak 属性。
- 原 Demo 不修改；失败的临时 Demo 不进入自动播放队列。
- 日志分别报告 `existing_module_rewritten`、`runtime_module_created`、`verified` 和明确的失败阶段。
