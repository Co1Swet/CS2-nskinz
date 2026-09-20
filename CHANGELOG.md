# Changelog / 更新记录

## Unreleased / 未发布

## v0.5.1-beta — 2026-09-20

- Added a `Maps` panel that discovers installed CS2 Workshop subscriptions and stages a selected map for the second parse/load of the same HLAE demo; no `.dem` or Workshop files are rewritten.
- 新增“地图更换”面板：自动识别本机已安装的 CS2 创意工坊订阅，并在同一 HLAE Demo 第二次解析／加载时应用目标地图；不改写 `.dem` 或工坊文件。
- Added an atomic fail-closed runtime chain covering Demo protobuf map/addon fields, client map KeyValues, manifest fallback and missing-model resource requests for the verified CS2 baseline.
- 新增原子化、失败关闭的运行时链：覆盖 Demo protobuf 地图／addon 字段、客户端地图 KeyValues、manifest 回退及缺失模型资源请求，并只针对已验证 CS2 基线启用。
- The map-remap implementation is adapted from the AGPL-3.0 MulNX_CS2 runtime behavior and second-load workflow without incorporating its file-rewriter path. This release includes the complete AGPL-3.0 license, provenance notice and Corresponding Source.
- 地图更换实现移植自 AGPL-3.0 的 MulNX_CS2 运行时行为与二次加载流程，但不纳入其文件改写路径；本版随附完整 AGPL-3.0 许可证、来源与改动声明及对应源码。
- Fixed the map bridge publication order so MinHook receives an executable detour instead of rejecting the first hook; the Maps panel now also fills the source-map field from the active runtime level.
- 修复地图桥代码页发布顺序，避免 MinHook 因 detour 尚不可执行而拒绝第一条 Hook；“地图更换”页现在也会从当前运行时 Level 自动回填原始地图。
- Re-audited the implementation against MulNX_CS2 commit `361008b208326d9e7916ff2cd58dd57a0492c400`: addon replacement is independent of the raw-map match, KeyValues no longer depends on DemoHeader ordering, and manifest/missing-vmdl fallback follows MulNX's always-on hook behavior. Added deferred per-stage replacement diagnostics.
- 按 MulNX_CS2 提交 `361008b208326d9e7916ff2cd58dd57a0492c400` 重新逐文件核对：addon 替换不再依赖原地图先命中，KeyValues 不再依赖 DemoHeader 调用顺序，manifest／缺失 vmdl 回退改为 MulNX 的常驻 Hook 行为，并增加非阻塞的分阶段替换日志。
- Added safe source-map generated-model suppression for Workshop map replacement and removed the experimental skybox VPK redirect after it caused fatal errors or stalled demo loading. Complete replacement of geometry baked into a compiled map remains map-dependent.
- 为创意工坊地图替换加入安全的原地图生成模型抑制；导致致命错误或 Demo 卡加载的 skybox VPK 重定向实验不随发布版提供。已烘焙进编译地图的几何体能否完整替换仍取决于目标地图资源。
- Updated the verified HLAE baseline to `2.192.2.0`; the CS2 baseline remains Steam Build `25218825`, Client/Server `2000908`, Patch `1.41.8.1`, Source revision `10981323`.
- 已验证 HLAE 基线更新至 `2.192.2.0`；CS2 基线仍为 Steam Build `25218825`、Client/Server `2000908`、Patch `1.41.8.1`、Source revision `10981323`。

This beta adds Workshop map-model replacement to the released feature set. Firearm skins retain the previous path; knife actions, stock gloves, radar edge pixels and map-specific baked geometry remain acceptance limitations. See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.1-beta.md).

## v0.5.0-beta — 2026-09-15

- Preserved the proven firearm skin path while integrating the current experimental knife-model/animation lifecycle and default-team glove transaction paths for HLAE demo-time updates.
- 保留已验证的枪械换肤路径，同时把当前刀模/动画生命周期与默认阵营手套事务路径整理为 HLAE Demo 播放期间更新的测试版。
- Hardened default T/CT glove mutation: engine-owned paint/seed/wear attributes are constructed before committing the item-view identity, and every partial failure restores the complete original state.
- 加固 T/CT 默认手套写入：先通过引擎构造涂装/种子/磨损属性，再提交物品视图身份；任一中途失败都会完整回滚，避免半初始化对象引发闪退。
- Preserved knife listeners across lifecycle adoption and separated weapon/HUD consumers to reduce pistol-slot placement and stale original-dagger animation reuse during demo reloads.
- 在刀实体生命周期接管时保留监听器，并分离武器与 HUD 消费者，针对重载 Demo 后刀模落入手枪位置及复用原皮匕首动作的问题进行修正。
- Marked this build as `v0.5.0-beta` in its filenames, panel title and startup log. Verified baseline: CS2 Steam Build 25218825 / ClientVersion 2000908 / Patch 1.41.8.1 / Source revision 10981323; HLAE 2.191.1.
- DLL 文件名、面板标题和启动日志均标记为 `v0.5.0-beta`。已验证基线：CS2 Steam Build 25218825 / ClientVersion 2000908 / Patch 1.41.8.1 / Source revision 10981323；HLAE 2.191.1。

This is a beta build: automated regression and isolated loading are release gates, while rendered glove/knife acceptance remains an explicit user-test boundary. See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.0-beta.md).

## v0.4.5 — 2026-09-02

- Fixed stock-firearm updates on clients where paint/seed/wear attributes do not yet exist by using the byte-verified native attribute-vector append path.
- 修复原皮枪械尚无涂装、种子和磨损属性时更新无效的问题，使用经过字节校验的引擎原生属性向量追加入口。
- Reused neutralized native tail records on repeated Update and retained fail-closed checks, avoiding duplicate attributes or unsafe writes on unknown layouts.
- 重复点击“更新”时复用已中和的原生尾部记录，并保留未知布局拒绝写入，避免属性重复增长或越界修改。
- Removed a duplicate full entity traversal from cosmetic polling by combining verified entity-ceiling discovery with cache population, addressing the reported periodic stutter.
- 合并经过验证的实体上限探测与缓存填充，移除外观轮询中的第二次全实体遍历，针对朋友机器上报告的间歇卡顿原因进行修复。
- Updated the verified compatibility baseline to CS2 Steam Build 25000182 / ClientVersion 2000899 / Patch 1.41.7.8 / Source revision 10948930.
- 更新兼容基线至 CS2 Steam Build 25000182 / ClientVersion 2000899 / Patch 1.41.7.8 / Source revision 10948930。
- Published statically linked bilingual binaries as `Lyan_nskinz v0.4.5 EN.dll` and `Lyan_nskinz v0.4.5 zh-CN.dll`. The current independent knife-model chain is included as experimental source/runtime and is not claimed fully stable for first-deploy, teammate-issued/dropped-knife or HUD handoffs.
- 以 `Lyan_nskinz v0.4.5 EN.dll` 与 `Lyan_nskinz v0.4.5 zh-CN.dll` 发布静态链接双语版本。当前独立刀模链随源码和运行时收录，但仍属实验功能，不宣称首次切刀、队友发刀/丢刀及 HUD 交接已完全稳定。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.4.5.md).

## v0.4.4 — 2026-08-31

- Fixed missing stock-weapon paint/seed/wear attributes and first-time native material initialization; stock AWP → BOOM is user-confirmed.
- 修复原皮枪械缺少涂装/种子/磨损属性及首次材质生成入口；原皮 AWP → 嘣已由用户实测确认。
- Shared across supported firearms, with owned-record rollback, stale-entity checks and bounded failure retries; no per-finish visual guarantee.
- 普通枪械共用修复，保留所属记录撤销、失效实体校验和失败重试限制；不宣称所有涂装已逐个实测。
- Nine targeted checks, all 46 native regression entries and isolated loader/rendering checks passed. Catalog coverage remains 34 guns / 1,449 compatible pairs.
- 9 个定向检查、46 项完整原生回归及隔离加载/渲染检查通过；目录覆盖 34 枪型 / 1,449 个合法组合。
- Published the exact tested English/Chinese DLLs with static `/MT` and embedded shaders. Paused knife-model continuation is excluded; CS2/HLAE baseline unchanged.
- 原样发布已测试中英 DLL，保留静态 `/MT` 和内嵌着色器；不混入暂停中的刀模续作，CS2/HLAE 基线不变。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.4.4.md).

## v0.4.3 — 2026-08-31

- Fixed target-paint modern/legacy mesh selection and high-slot first-person HUD lookup, including M4A4 Full Throttle → The Emperor.
- 修复目标涂装新旧网格选择及高位第一人称 HUD 漏查，包括破浪狂飙 → 皇帝显示原皮。
- Added exact gun/paint compatibility filtering and runtime/config validation without deleting the full catalog or silently selecting vanilla.
- 增加精确枪型涂装筛选与运行时/配置校验，不删减完整目录，不静默改为原皮。
- Pending HUD bindings defer writes; verified gun entity/render generations invalidate stale refresh cache without repeated stable refreshes.
- HUD 未就绪时延后写入；验证枪械/渲染实体代际，避免复用旧刷新缓存及稳定对象重复刷新。
- Retained isolated IME/search and portable loading fixes. Independent knife-model source is included but its runtime adapter remains read-only.
- 保留输入法/搜索隔离及单 DLL 加载方案；包含独立刀模开发源码，运行时适配器仍只读。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.4.3.md).

## v0.4.2 — 2026-08-30

- Embedded precompiled UI shaders; removed the D3DCOMPILER_47 runtime dependency while retaining static MSVC `/MT`.
- 面板着色器预编译后内嵌，移除 D3DCOMPILER_47 运行时依赖，保留静态 MSVC `/MT`。
- Preserved static CRT thread notifications and added import audits and isolated loader/renderer regression checks.
- 保留静态 CRT 所需线程通知，新增依赖审计及独立加载/渲染验证。
- Documented full paths and correct quotes. The user confirmed successful loading after correcting the command; no missing-VC++ root cause is claimed.
- 明确完整路径与正确引号；用户修正指令后确认加载成功，不将此前问题误归因为缺少 VC++ 运行库。
- Published the exact verified DLLs; cosmetic runtime and the CS2/HLAE compatibility baseline remain unchanged from v0.4.1.
- 发布已验证的原始 DLL，换肤逻辑及 CS2/HLAE 基线沿用 v0.4.1。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.4.2.md).

## v0.4.1 — 2026-08-30

- Fixed IME/search-input crashes in CS2; the complete panel runs in an isolated host with the full catalog.
- 修复输入法及皮肤搜索输入导致 CS2 闪退；完整面板独立运行，保留全量皮肤目录。
- Fixed dynamic weapon attributes, redundant knife refreshes and the finish-only model selector.
- 修复枪械动态涂装、刀材质重复刷新及“仅饰面”显示模型选项的问题。
- Removed blocking mesh-update waits, repeated catalog copies and synchronous frame-callback file logging. Full-demo FPS verification remains pending.
- 移除网格阻塞等待、重复目录复制与帧回调内同步写日志；整段 Demo 帧率复测仍待完成。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.4.1.md).
