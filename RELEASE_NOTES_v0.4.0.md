# nSkinzCS2 v0.4.0

## 兼容基线

- Steam app build ID: `24957633`
- CS2 ClientVersion: `2000897`
- PatchVersion: `1.41.7.8`
- Source revision: `10942762`
- Game build date: `2026-08-26`
- `client.dll` SHA-256: `84BCAE9DB379BED5ACB3D81228ABEF86FBD61B6CDB2B3ACCDC5DD0F32E8F89F9`
- HLAE: `2.191.1` / AfxHookSource2 `0.40.1`

CS2 更新后内存布局可能变化；如果版本不匹配，DLL 会记录签名校验失败并拒绝不确定的写入。

## 下载

- `nSkinzCS2.dll`: English UI
- `nSkinzCS2.zh-CN.dll`: 简体中文 UI，并从当前 CS2 VPK 加载中文武器、刀、手套和皮肤名称

两份 Release DLL 均使用 MSVC `/MT` 静态运行库，不需要另行安装 Visual C++ Redistributable、Python、Visual Studio 或 Windows SDK。

## 加载

仅用于 HLAE `-insecure` 的离线 demo 播放，不要用于 VAC/联机服务器。

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.dll"
```

中文版：

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.zh-CN.dll"
```

按 `Insert` 显示窗口，在 demo 已加载后配置 XUID 并点击 `Update`。

## 本版变更

- 刀皮 finish-only 路径：保留 demo 玩家原刀模，替换 paint kit / seed / wear。
- 刀切换的稳态刷新防护：阻止每帧重复的材质初始化调用。
- 武器、刀、手套、挂件菜单区分 `- (不修改)` 与 `原皮（无涂装）` / `无挂件（移除）`。
- 多 XUID 分离配置，可对不同玩家同时应用相同或不同的枪械、刀和手套皮肤。
- 新增简体中文构建，中文皮肤名称直接来自安装的 CS2 `csgo_schinese.txt`。
- 新增当前 CS2 签名和 PE 兼容性诊断。

## 实测摘要

- Dust2：三名 XUID 同时持枪时，AK 表面淬火 `44/seed 661`、AWP 二西莫夫 `279`、AK 震生颠覆 `600` 全部通过运行时回读。
- Dust2：三名 XUID 同时持刀时，骷髅匕首多普勒 `415`、蝴蝶刀渐变之色 `38/seed 420`、蝴蝶刀屠夫 `59/seed 77` 全部通过回读。
- Ancient `g161-20260826211557183178228_de_ancient`：爪子刀 `44/seed 1`、蝴蝶刀 `44/seed 321`、M9 刺刀 `44/seed 999` 分别通过 XUID/刀模/模板/种子回读。
- 英文和中文工作副本构建成功，本地原生测试套件全部通过。

## 已知限制

当 demo 刀已经持有编译后的旧自定义材质时，选择“原皮（无涂装）”仍可能在第一人称视角保留旧材质。正数 paint kit 刀皮已验证；该刀原皮缓存问题不在 v0.4.0 声称已修复。
