# Changelog / 更新记录

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
