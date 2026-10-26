# Changelog / 更新记录

## v0.6.4 — 2026-10-26

- Added Temporary Demo reconstruction for keychain replacement, addition and explicit removal. Existing position/seed are preserved; new mounts use model-derived DefaultLocation regions for 34 firearm definitions. Keychain-only requests preserve unrequested paint, stickers, quality and names.
- 挂件替换、新增和明确清除统一接入临时 Demo 重建；保留已有位置和 seed，新增采用 34 种枪械模型的 DefaultLocation 区域。独立挂件请求保留未请求的枪皮、贴纸、品质和名称。
- Disabled live keychain mutation, module creation and keychain-only entity scans. Idle/generated-Demo factory callbacks now return before copying configuration; the external rewriter runs below normal CPU priority.
- 关闭实时挂件写入、模块创建和挂件专用扫描；空闲及生成 Demo 的工厂回调在复制配置前返回，外置重写进程降低 CPU 优先级。
- Verified four independent recordings: 1,317 weapon handles, 506,431 target records and 3,106 Created snapshots for replacement/addition and again for removal, plus 202 independent seek readbacks. A mixed firearm/five-sticker/keychain/T-CT-glove request also passed. Rendered keychain appearance and actual gameplay frame-time improvement remain in-game acceptance items.
- 四份独立录制通过回归：1,317 把目标枪械、506,431 条挂件记录、3,106 个创建快照；清除阶段再次验证相同规模，另有 202 次独立跳转读回。枪皮、五槽贴纸、挂件及 T／CT 手套混合请求也通过。挂件画面和实际游戏帧时间改善仍需实机验收。

See [bilingual release notes](docs/releases/v0.6.4.md) and [implementation and regression evidence](docs/development/keychain-temporary-demo-20261026.md).

## v0.6.3 — 2026-10-25

- Fixed default T/CT gloves disappearing after seeking a Temporary Demo: every independent Created/full-packet record now materializes its complete glove state even when its Pawn handle and team were seen earlier. Late team assignment remains supported.
- 修复临时 Demo 跳转后默认 T／CT 手套重建失效：每个独立 Created／完整快照均重新写入完整手套状态，保留队伍字段晚于实体创建到达的兼容逻辑。
- Added per-record verification of all requested glove fields, including independent seek snapshots. Default, unreadable and generated-Demo glove carriers are now strictly read-only in the injected runtime; mismatches require a new Temporary Demo and cannot enter live writes, native refresh or Wearable creation.
- 新增逐记录手套字段验证，覆盖独立跳转快照；默认、未读明及生成 Demo 的手套载体在注入端均严格只读，不匹配时要求重新生成临时 Demo，禁止进入实时写入、原生刷新或 Wearable 创建。
- Validated four independent source recordings with 24 default-glove player/recording combinations, 1,894,511 target records, 702 Created snapshots and 132 independent seek readbacks, covering both T and CT in each recording.
- 跨四份独立录制完成回归：覆盖 24 人次默认手套玩家、1,894,511 条目标记录、702 个 Created 快照及 132 次独立跳转读回，每份均覆盖 T／CT 两方。

See [bilingual release notes](docs/releases/v0.6.3.md) and [diagnosis and real-Demo regression evidence](docs/diagnostics/default-glove-seek-snapshots.md).

## v0.6.2 — 2026-10-23

- Rebased every guarded current-CS2 native path on Steam Build `26144732`, Client/Server `2000935`, Patch `1.41.9.2`, Source revision `11162119`. Knife serialization, model/material refresh, skeleton resources, animation graph/arms lifecycle, stock-glove refresh and HUD consumers now use the October 22 client layout with exact call/RIP-window verification.
- 将所有受保护的当前 CS2 原生路径迁移到 Steam Build `26144732`、Client/Server `2000935`、Patch `1.41.9.2`、Source revision `11162119`。刀具序列化、模型／材质刷新、骨架资源、动画图／手臂生命周期、默认手套刷新及 HUD 消费者均改用 10 月 22 日客户端布局，并保留精确调用／RIP 指令窗验证。

- Fixed the evidence-confirmed stock/default CT and T glove failure first isolated on CS2 `2000934`. The Temporary Demo serializes and verifies the complete Pawn `m_EconGloves` definition, identity, finish, changed counter and bodygroup fields; runtime readback confirmed `definition=5030 / paint=10048`. An exact generated-demo match is now accepted as the authoritative transaction without a second live field write or HUD/native refresh. This removes the discarded development path that created the correct `glove_sporty.vmdl` only transiently and then let the HUD chain retire it. Non-generated or non-matching carriers still use the guarded live fallback.
- 修复最初在 CS2 `2000934` 上定位、并由运行日志确认的默认 CT／T 原皮手套失效。临时 Demo 已完整序列化并验证 Pawn `m_EconGloves` 的定义、身份、涂装、变更计数和 bodygroup 字段，运行时实读也确认为 `definition=5030 / paint=10048`。现在对完全匹配的重建 Demo 载体直接认定序列化事务完成，不再叠加第二次实时字段写入或 HUD／原生刷新。这移除了开发阶段“瞬时创建正确 `glove_sporty.vmdl`、随后又被 HUD 链退役”的冲突路径。非重建 Demo 或字段不匹配的载体仍使用受保护的实时兜底。
- Fixed the subsequent demo-reload access violation using the captured `cs2_2026_1022_203619_0_accessviolation.mdmp` and runtime log. The fault occurred in `client.dll` while destroying the old Pawn, after the previous DLL had attached runtime-owned attributes to its readable definition-zero `m_EconGloves` slot. The runtime now treats that exact stock slot as rewrite-only: it neither writes the embedded item view nor creates/mirrors a live `CEconWearable`; only a known non-zero glove already materialized by the generated demo may enter the Pawn carrier path.
- 根据实取的 `cs2_2026_1022_203619_0_accessviolation.mdmp` 与运行日志修复随后出现的 Demo 重载访问冲突。故障发生在 `client.dll` 销毁旧 Pawn 时；此前 DLL 曾向可读但 definition=0 的 `m_EconGloves` 空槽挂接运行时属性。现在该默认空槽被严格设为“仅允许临时 Demo 重写”：不再实时写入嵌入 ItemView，也不再创建／镜像实时 `CEconWearable`；只有生成 Demo 已物化的已知非零手套定义才能进入 Pawn 载体路径。
- Adapted the isolated Workshop-radar compiler to the updated CS2 toolchain, which now requires the mounted `core/pak01_dir.vpk` index. The cache links the installed directory indexes read-only and includes them in its environment fingerprint; it never copies or edits the large game archives.
- 适配今日 CS2 工具链新增的创意工坊雷达编译依赖：`resourcecompiler` 现在启动时要求挂载 `core/pak01_dir.vpk` 索引。隔离缓存会只读硬链接本机目录索引并纳入环境指纹，不复制或修改大型游戏资源包。
- Revalidated HLAE `2.192.8` / AfxHookSource2 `0.41.8`, all 17 installed-client signatures, the complete native suite, 37 installed Workshop-map entries, native radar VTEX compilation, 14 demo-rewriter tests, a real 97.7 MB glove rewrite, six isolated DLL-loading cases and the WARP render gate. The current installed catalog contains 1,408 weapon finishes, 74 glove finishes, 11,642 stickers and 144 keychains.
- 已重新验证 HLAE `2.192.8` / AfxHookSource2 `0.41.8`、17 项客户端签名、完整原生回归、37 个已安装工坊地图条目、雷达原生 VTEX 编译、14 项 Demo 重写器测试、97.7 MB 真实 Demo 手套重写、6 项隔离 DLL 加载及 WARP 渲染门禁。本机当前目录包含 1,408 个枪械涂装、74 个手套涂装、11,642 张贴纸和 144 个挂件。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.6.2.md).

## v0.6.1 — 2026-10-15

- Rebased all guarded current-CS2 native paths on Steam Build `25947352`, Client/Server `2000932`, Patch `1.41.9.1`, Source revision `11131657`. Knife serialization, model/material refresh, skeleton resources, animation graph/arms lifecycle and stock-glove visual refresh now use the October 14 client layout with exact call/RIP-window verification.
- 将所有受保护的当前 CS2 原生路径迁移到 Steam Build `25947352`、Client/Server `2000932`、Patch `1.41.9.1`、Source revision `11131657`。刀具序列化、模型／材质刷新、骨架资源、动画图／手臂生命周期及默认手套视觉刷新均改用 10 月 14 日客户端布局，并保留精确的调用／RIP 指令窗验证。
- Preserved the full v0.6.0 sticker feature set: five logical slots, creation of absent legacy/modern fields, model-derived placement preview, sliders, and throttled live synchronization. The installed 2026-10-15 catalog parses as 1,408 weapon finishes, 74 glove finishes, 11,642 stickers and 144 keychains.
- 完整保留 v0.6.0 贴纸功能：五个逻辑槽、缺失 legacy／现代字段创建、模型投影位置预览、滑轨及节流实时同步。2026-10-15 本机目录解析结果为 1,408 个枪械涂装、74 个手套涂装、11,642 张贴纸和 144 个挂件。
- Reduced runtime overhead without changing feature behavior: entity RTTI/classification is cached per generation instead of repeatedly scanned, and the isolated control panel reuses immutable catalog-derived UI lists instead of rebuilding them on every frame.
- 在不改变功能行为的前提下降低运行开销：实体 RTTI／分类结果按代际缓存，避免重复扫描；独立控制面板复用不可变目录生成的 UI 列表，不再每帧重建。
- Revalidated demo rewriting, firearm materials, stock gloves, knives, stickers and Workshop-map radar processing against the installed game. All 16 compatibility-manifest signatures matched exactly once; the full native suite, native radar VTEX compilation and portable-load gates pass. Rendered results in a specific demo remain an in-game acceptance boundary.
- 已针对本机当前游戏重新验证 Demo 重写、枪械材质、默认手套、刀具、贴纸和创意工坊地图雷达处理。兼容清单 16 项签名均唯一匹配；完整原生回归、雷达原生 VTEX 编译及便携加载门禁通过。具体 Demo 的最终画面仍需游戏内验收。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.6.1.md).

## v0.6.0-beta — 2026-10-10

- Added opt-in CS2 free sticker placement for firearm slots. Preset mode remains the default and preserves the demo's recorded transform; custom mode writes wear, scale, rotation, per-slot offset delta X/Y and schema together.
- 新增可选的 CS2 枪械贴纸自由位置。默认仍为预设模式并保留 Demo 已记录的变换；自定义模式会整体写入磨损、缩放、旋转、相对槽位基准的 offset 增量 X/Y 与 schema。
- Added a draggable two-dimensional placement guide with scale/rotation feedback and throttled live synchronization to the loaded demo. The game view remains authoritative; when a source slot lacks modern placement nodes, generate a Temporary Demo once to materialize them before using live preview.
- 新增可拖拽的二维位置参考画布，可同步显示缩放／旋转，并以节流频率把调整实时发布到当前 Demo。最终效果以游戏画面为准；若源槽位缺少现代位置节点，需先生成一次临时换肤 Demo 补齐字段，再使用实时预览。
- The Temporary Demo rewriter now understands CS2's legacy sticker, modern offset and schema attribute families (the five exposed nSkinz slots use `113–132`, `278–287` and `290–294`), can append any absent fields without hard-coded vector sizes, and reparses every requested component for exact 32-bit verification.
- 临时 Demo 重写器现已识别 CS2 的 legacy 贴纸、现代偏移与 schema 属性族（nSkinz 当前公开的五个槽位使用 `113–132`、`278–287` 与 `290–294`），可在不写死向量长度的情况下补齐任意缺失字段，并在输出后二次解析，对每个请求字段执行精确 32 位校验。
- The five-slot canvas now uses the exact selected firearm silhouette for all 34 supported definitions instead of a generic weapon-family drawing. AK-47, M4A4, M4A1-S and every other model are distinct, all silhouettes face muzzle-left, and selected/configured/unused slots remain yellow/white/dim respectively.
- 五槽位画布现已为 34 个受支持 definition 显示当前具体枪械轮廓，不再使用步枪／手枪等通用大类图。AK-47、M4A4、M4A1-S 等型号均可区分，枪口统一朝左，选中／已配置／未使用槽位仍分别为黄色／白色／暗色。
- Fixed a second placement mismatch revealed by live CS2 validation: Econ attributes 278-287 are per-slot deltas, while the material proxy adds the weapon's `g_vStickerNOffset`. The editor had written that base as though it were an absolute Econ value, so CS2 applied it twice and pushed stickers off the previewed point. The canvas now projects `material base + stored delta`, dragging subtracts the base before serialization, and version-7 development configs migrate once to delta storage.
- 根据 CS2 实机验证修复第二处位置错位：Econ 属性 278–287 是相对槽位基准的增量，材质代理还会叠加枪械的 `g_vStickerNOffset`。此前编辑器把该基准当成绝对 Econ 值写入，导致游戏重复叠加并把贴纸推离预览点。现在画布投影“材质基准＋存储增量”，拖拽序列化前会减去基准，并将 version 7 开发配置一次性迁移为增量。
- Replaced the still-drifting silhouette-anchor approximation with projection data derived from the current CS2 VMDLs. Every slot converts its effective material offset to UV1 (`0.5 + offset`), locates its real model projection triangle, and uses that triangle's UV1-to-muzzle-left side-projection matrix plus the exact inverse for dragging. This fixes the previously reversed X/Y movement and the severely understated per-weapon Y rate. Zero-scale material placeholders are not reported as native presets; a missing fifth preset is sampled from a real surface triangle on that weapon.
- 将仍会错位的轮廓锚点近似方案替换为从当前 CS2 VMDL 模型生成的真实投影数据。每个槽位现在把叠加基准后的有效材质 offset 转为 UV1（`0.5 + offset`），定位其实际模型投影三角形，并使用该三角形到枪口朝左侧投影的矩阵及精确逆矩阵处理拖拽；由此修复此前 X／Y 移动方向均相反、且不同枪型纵向倍率被严重低估的问题。缩放为 0 的材质占位不再被标为原生预设；缺失的第 5 预设会从该枪型真实表面三角形采样起点。
- Rebased the guarded stock-glove visual refresh on CS2 Steam Build `25815307`, Client/Server `2000930`, Patch `1.41.9.0`, Source revision `11106093`. The updated native callback, HUD consumer and identity-root evidence restores the generated-demo path for default CT/T glove carriers without weakening byte-level validation.
- 将带保护的默认手套视觉刷新迁移到 CS2 Steam Build `25815307`、Client/Server `2000930`、Patch `1.41.9.0`、Source revision `11106093`；更新原生回调、HUD 消费者和实体标识根节点证据，在不降低字节级验证强度的前提下恢复默认 CT／T 手套的生成 Demo 路径。
- Pinned `de_mirage_water` to an embedded, audited `720×720` alpha reference instead of guessing large enclosed dark regions. Its `298,510` fully transparent pixels and `44,755` antialiased edge pixels are reproduced exactly while RGB continues to come from the installed Workshop texture; the generic dark-road radius intentionally does not alter this map.
- `de_mirage_water` 现固定使用内嵌并核验过的 `720×720` Alpha 基准，不再猜测大面积封闭暗区。基准图的 `298,510` 个完全透明像素和 `44,755` 个抗锯齿边缘像素被精确复现，RGB 仍取自本机安装的工坊纹理；通用暗色道路恢复半径不会再改变该地图。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.6.0-beta.md).

## v0.5.8 — 2026-10-03

- Temporary Demo can now create requested firearm sticker slots that are absent from the source demo. The rewriter grows the Econ dynamic-attribute vector, writes the complete ID/wear/scale/rotation record at the weapon's native preset position, preserves existing CS2 free-placement data, and reparses the output to verify every requested slot.
- “临时换肤 Demo”现在可创建源 Demo 中不存在的枪械贴纸槽。重写器会扩展 Econ 动态属性向量，按武器原生预设位置写入完整的 ID／磨损／缩放／旋转记录，保留已有 CS2 自由摆放数据，并在输出后二次解析验证每一个请求槽位。
- Live Update remains an existing-slot replacement path. v0.5.8 deliberately does not create or edit CS2 free-placement offset/schema attributes; newly created slots use their native preset positions.
- 实时“更新”仍只替换已有槽位。v0.5.8 明确不创建或修改 CS2 自由摆放的偏移／Schema 属性；新建槽位使用原生预设位置。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.8.md).

## v0.5.7 — 2026-10-03

- Fixed transparent Workshop radars such as `INFERNO REFORGED` being skipped as “already transparent” even though their packaged VTEX could still render an opaque black field in `mirv_pov`. Existing source alpha is now normalized through the cache-isolated native overlay path, and the radar background-opacity control also synchronizes `cl_hud_radar_background_alpha` on a safe render frame.
- 修复 `INFERNO REFORGED` 等工坊雷达因解码后存在 Alpha 而被误判为“无需处理”，但在 `mirv_pov` 中仍显示不透明黑底的问题。现在会通过独立缓存的原生 VTEX 覆盖路径规范化已有 Alpha，并在安全渲染帧同步 `cl_hud_radar_background_alpha` 与面板中的黑底不透明度。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.7.md).

## v0.5.6 — 2026-10-02

- Fixed valid current-CS2 demos failing with `serialized entity length count mismatch` by dynamically mapping the `serialized_entities` table to update/create commands that actually carry field payloads. The implementation accepts arbitrary sparse counts rather than hard-coding the reported `117 / 191` case, while retaining full-entry and delete-inclusive compatibility.
- 修复当前 CS2 合法 Demo 因 `serialized entity length count mismatch` 而重写失败：现在动态把 `serialized_entities` 长度表映射到真正携带字段数据的 Update/Create 命令。实现兼容任意稀疏数量，不写死截图中的 `117 / 191`，同时保留全条目及包含 Delete 的布局。
- Moved runtime state, schema/entity binding, application statistics, diagnostics and catalog warnings into a collapsed-by-default diagnostics panel.
- 将运行时状态、Schema／实体绑定、应用统计、诊断及 Catalog 警告整合到默认收起的调试信息栏。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.6.md).

## v0.5.5 — 2026-10-01

- Updated the verified baseline to CS2 Steam Build `25640462`, Client/Server `2000922`, Patch `1.41.8.8`, Source revision `11064488`, including the shifted native glove refresh/HUD/entity-root evidence and compact weapon-material generator ABI.
- 已验证基线更新至 CS2 Steam Build `25640462`、Client/Server `2000922`、Patch `1.41.8.8`、Source revision `11064488`，同步迁移手套视觉刷新／HUD／实体块证据与紧凑栈帧枪械材质生成 ABI。
- Fixed Temporary Demo output that could stall on the loading/progress screen by preserving field-path bit encoding and synchronizing the per-entry `serialized_entities` bit-length table after rewrites.
- 修复临时 Demo 因字段路径位流和逐实体 `serialized_entities` 位长度表不同步而卡在加载进度条的问题。
- Existing firearm/glove paint attributes are now overwritten in place instead of bypassed. All rebuilt firearms serialize verified legacy/modern mesh selection; knife routing remains unchanged.
- 已有枪械／手套涂装属性现在原位覆盖而非跳过；所有重建枪械都会写入经验证的新旧网格选择，刀具路径保持不变。
- Added firearm existing-sticker image replacement while preserving the demo's wear, scale, rotation and CS2 free placement. Missing sticker slots are not created.
- 新增枪械已有贴纸原位换图案，保留 Demo 中的磨损、缩放、旋转与 CS2 自由摆放；不会创建缺失槽位。
- Added a `de_mirage_water` radar profile that restores its wider bounded dark roads without retaining the outside matte.
- 为 `de_mirage_water` 雷达增加专用安全恢复参数，补回更宽的封闭暗色道路，同时不保留外部底色。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.5.md).

## v0.5.4 — 2026-09-30

- The temporary-demo knife path no longer copies or requires the target animation skeleton row from the source demo. It validates the compiled target skeleton in the installed CS2 `game/csgo` loose content or `pak01_dir.vpk`, then writes the standard `AnimAssetData` loader metadata.
- 临时 Demo 换刀不再从源 Demo 复制或要求目标动画骨骼记录；现在从已安装 CS2 的 `game/csgo` 松散文件或 `pak01_dir.vpk` 验证目标编译骨骼，再写入标准 `AnimAssetData` 加载元数据。
- Retained hybrid cosmetic routing: already-painted firearms and gloves stay on the established live path, while stock/default firearms, T/CT gloves (`5028`/`5029`) and T/CT knives (`59`/`42`) are materialized in the generated demo before its second playback.
- 保留混合换肤路由：已有涂装的枪械和手套继续走原实时路径；原皮枪械、T/CT 默认手套（`5028`/`5029`）与 T/CT 默认匕首（`59`/`42`）在生成的 Demo 中补齐后再二次回放。
- Fixed stock AK-47 finish activation on CS2 2000919 by verifying the updated native material-generator entry and running material initialization once per entity generation with a stable cache instead of per-frame refreshes.
- 修复 CS2 2000919 下原皮 AK-47 涂装不显示的问题：验证新版原生材质生成入口，每个实体代际仅初始化一次并复用稳定缓存，不再逐帧刷新。
- Added default/custom name-tag serialization for firearms, knives and gloves. An empty custom value uses `nSkinz CS2`.
- 为枪械、刀和手套加入默认／自定义名称标签序列化；未填写自定义名称时使用 `nSkinz CS2`。
- Added offline Rust regression coverage for installed loose/VPK animation-resource lookup and the exact weapon animation metadata layout. Targeted weapon-material, cosmetic-rewrite and isolated x64 loader/render checks pass on the CS2 2000919 / HLAE 2.192.6 baseline.
- 新增已安装松散文件／VPK 动画资源查找及标准武器动画元数据的离线 Rust 回归；CS2 2000919 / HLAE 2.192.6 基线下武器材质、饰品重写和独立 x64 加载／渲染门禁通过。

See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.4.md).

## v0.5.3-beta — 2026-09-27

- Updated the verified HLAE baseline to `2.192.6` / AfxHookSource2 `0.41.6` (`0E1378B81B5B82974EA8DD5C3E701E924D63C452927B50D64C1580F746D01F8B`).
- 已验证 HLAE 基线更新为 `2.192.6` / AfxHookSource2 `0.41.6` (`0E1378B81B5B82974EA8DD5C3E701E924D63C452927B50D64C1580F746D01F8B`)。
- Reworked Workshop radar masking so black tolerance is a real signed user correction, expanded bounded dark-road recovery to 32 pixels, migrated the destructive `24/4` defaults to road-safe `12/12`, and content-addressed every generated overlay to prevent stale slider results.
- 重做创意工坊雷达遮罩：黑底容差现在是真实的双向手动偏移，受边界约束的暗色道路恢复扩展到 32 像素，旧版会过度删路的 `24/4` 默认值迁移为 `12/12`，并为每次生成的覆盖使用内容寻址资源名，避免滑块结果被旧缓存覆盖。

- Added per-texture neutral-matte detection for Workshop radar overlays. A secondary opaque gray peak above the configured cutoff is now detected automatically, fixing the square/grid background retained by maps such as Ancient Night without forcing every map to use a destructive high threshold.
- 为创意工坊雷达加入按纹理检测的中性暗底阈值：可自动识别高于手动阈值的第二个不透明灰阶峰，修复 Ancient Night 等地图的方形／网格黑底，不再迫使所有地图使用会吞掉道路的高阈值。
- Reworked dark-detail recovery so recovered corridors and antialiased boundaries inherit RGB from a trusted neighbouring road instead of restoring the source matte as opaque black. On the installed `de_inferno_water` audit texture, opaque pure-black output pixels dropped from 2,229 to zero while road coverage stayed intact.
- 重做深色细节恢复：被恢复的窄路与抗锯齿边界现在继承可信相邻道路的 RGB，不再把原纹理的黑底补成不透明黑渣。本机 `de_inferno_water` 审计纹理中的不透明纯黑输出像素由 2,229 降为 0，道路覆盖保持完整。
- Added real installed-Workshop regression coverage for `de_ancient_night` and retained the existing Nirage, Nuke Rainy, Mirage/Dust2/Inferno Water and All-in-One radar gates. Runtime logs now report configured/effective tolerance and detected neutral-matte statistics.
- 新增本机真实创意工坊 `de_ancient_night` 回归，并保留 Nirage、Nuke Rainy、Mirage/Dust2/Inferno Water 与 All-in-One 雷达门禁；运行日志新增配置／实际阈值及中性暗底统计。
- The verified compatibility baseline is CS2 Steam Build `25537370`, Client/Server `2000918`, Patch `1.41.8.5`, Source revision `11039926`, HLAE `2.192.6` / AfxHookSource2 `0.41.6`. Full native regression and isolated x64 loading checks pass.
- 已验证兼容基线为 CS2 Steam Build `25537370`、Client/Server `2000918`、Patch `1.41.8.5`、Source revision `11039926`、HLAE `2.192.6` / AfxHookSource2 `0.41.6`；完整原生回归与独立 x64 加载检查通过。

This beta is the Workshop-radar transparency, road-preservation and edge-recovery update for the CS2 2000918 / HLAE 2.192.6 baseline. See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.3-beta.md).

## v0.5.2-beta — 2026-09-27

- Rebased the current source and local beta binaries on HLAE `2.192.5.0` / AfxHookSource2 `0.41.5` and CS2 Steam Build `25537370`, Client/Server `2000918`, Patch `1.41.8.5`, Source revision `11039926`.
- 将当前源码与本地测试 DLL 适配到 HLAE `2.192.5.0` / AfxHookSource2 `0.41.5`，以及 CS2 Steam Build `25537370`、Client/Server `2000918`、Patch `1.41.8.5`、Source revision `11039926`。
- Migrated the independent knife serialization, model/recollection, skeleton-resource, weapon-type graph, arms lifecycle and material-transfer evidence to the new client/animationsystem layout. The firearm implementation remains the existing attribute/material path, with only the engine's shifted composite-material owner layout updated.
- 迁移独立刀模链的动画序列化、模型／重收集、骨架资源、weapon-type 动画图、手臂生命周期和材质传递证据；枪械仍沿用既有属性／材质路径，只同步游戏移动后的复合材质所有者布局。
- Migrated the default-glove path to Pawn `m_EconGloves=0x1770`, `m_bNeedToReApplyGloves=0x176D`, `m_nEconGlovesChanged=0x1D20`, the native output component at `Pawn+0x15F8`, and the current HUD consumer. Added installed-PE evidence for all seven glove windows.
- 将默认手套路径迁移到 Pawn `m_EconGloves=0x1770`、`m_bNeedToReApplyGloves=0x176D`、`m_nEconGlovesChanged=0x1D20`、`Pawn+0x15F8` 原生输出组件及当前 HUD consumer，并新增全部七处手套指令窗的已安装 PE 验证。
- Full Release regression, all 16 installed-CS2 manifest entries (14 required skin/runtime gates plus two reference entries), and strict knife/glove file-only ABI checks pass. Actual second-load deploy/inspect animation and stock-glove rendering remain an in-game demo acceptance boundary.
- Release 全量回归、本机 CS2 兼容清单全部 16 项（14 项换肤／运行时必需门控及 2 项参考证据）及刀／手套严格文件 ABI 检查均通过；二次加载后的切刀／检视动作与默认手套画面仍需在实际 Demo 中验收。
- Updated the map-remap GameSession parser from MulNX upstream `f2466e18e26a8fc3daf4ac7945bc45220ba95993`: the signature now masks protobuf branch displacements and `CSVCMsg_GameSessionConfiguration::s1_mapname` uses the current `+0x40` slot. This prevents the full map hook set from failing closed after the CS2 update.
- 按 MulNX 上游 `f2466e18e26a8fc3daf4ac7945bc45220ba95993` 更新地图替换的 GameSession 解析器：特征码改为屏蔽 protobuf 分支位移，`CSVCMsg_GameSessionConfiguration::s1_mapname` 使用当前 `+0x40` 字符串槽，避免 CS2 更新后整组地图 Hook 因单条旧特征失配而关闭。

This beta is the CS2 2000918 / HLAE 2.192.5 compatibility release. Firearm skins keep the previous implementation; map replacement, knife model/action and stock-glove rendering remain demo-specific acceptance items. See [full bilingual release notes / 完整中英发布说明](docs/releases/v0.5.2-beta.md).

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
