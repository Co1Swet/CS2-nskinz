# Changelog / 更新记录

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
