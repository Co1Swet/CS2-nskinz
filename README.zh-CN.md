<p align="center"><img src="docs/assets/nskinz-banner.svg" alt="nSkinzCS2 HLAE 离线 Demo 皮肤工具" width="900"></p>

<h3 align="center">面向 HLAE Demo 回放的 Counter-Strike 2 离线外观工具</h3>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/blob/main/README.md"><img src="https://img.shields.io/badge/English-README-555?style=flat-square" alt="English README"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/blob/main/README.zh-CN.md"><img src="https://img.shields.io/badge/简体中文-README-1f6feb?style=flat-square" alt="简体中文 README"></a>
</p>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/latest"><img src="https://img.shields.io/github/v/release/Lyan-edit/CS2-nskinz?style=flat-square&label=release" alt="最新版本"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases"><img src="https://img.shields.io/github/downloads/Lyan-edit/CS2-nskinz/total?style=flat-square&label=下载量" alt="下载量"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz"><img src="https://img.shields.io/github/stars/Lyan-edit/CS2-nskinz?style=flat-square" alt="Stars"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/network/members"><img src="https://img.shields.io/github/forks/Lyan-edit/CS2-nskinz?style=flat-square" alt="Forks"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues"><img src="https://img.shields.io/github/issues/Lyan-edit/CS2-nskinz?style=flat-square" alt="Issues"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square" alt="C++20">
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/HLAE-离线%20Demo-6f42c1?style=flat-square" alt="HLAE 离线 Demo">
</p>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.0/nSkinzCS2.dll"><img src="https://img.shields.io/badge/下载-英文%20DLL-2ea043?style=for-the-badge" alt="下载英文 DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.0/nSkinzCS2.zh-CN.dll"><img src="https://img.shields.io/badge/下载-中文%20DLL-0969da?style=for-the-badge" alt="下载中文 DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/tag/v0.4.0"><img src="https://img.shields.io/badge/版本说明-v0.4.0-8250df?style=for-the-badge" alt="版本说明"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues/new/choose"><img src="https://img.shields.io/badge/提交问题-f85149?style=for-the-badge" alt="提交问题"></a>
</p>

> **安全提示：** nSkinzCS2 只用于通过 HLAE、带 `-insecure` 启动的离线 Demo 回放。请勿加载到 VAC 保护服务器或任何联机服务器。超出此范围使用造成的账号风险由使用者自行承担。

## nSkinzCS2 是什么？

nSkinzCS2 是一个 Windows x64 DLL，用于在观看 Counter-Strike 2 Demo 时修改当前观战玩家的外观。它通过 HLAE 的 `mirv_loadlibrary` 加载，读取本机 CS2 目录中的物品目录，并通过 Win32 + D3D11 + Dear ImGui 控制面板，按玩家 XUID 安全地应用外观覆盖。

<p align="center"><img src="docs/assets/control-panel.png" alt="nSkinzCS2 控制面板" width="900"></p>

## 功能矩阵

| 模块 | v0.4.0 支持情况 | 说明 |
| --- | --- | --- |
| 枪械 | 涂装、种子、磨损、品质、名称标签和贴纸字段 | 按观战玩家 XUID 与基础物品定义隔离配置。 |
| 刀 | 仅换涂装、种子和磨损 | 保留 Demo 中的刀模；骷髅匕首、M9 刺刀、蝴蝶刀和爪子刀的正数涂装已验证。 |
| 手套 | T/CT 默认手套、刀模和涂装 | 已在当前兼容版本验证默认手套定义 `5028`、`5029`。 |
| 挂件 | 修改或清除已有实时模块 | 对没有挂件模块的物品创建模块仍属实验功能。 |
| 原皮 / 无涂装 | 独立的 `原皮（无涂装）` 选项 | 与 `-（不修改）` 不同；部分 Demo 刀的第一人称旧材质可能残留。 |
| 语言 | 英文和简体中文 DLL | 中文版从本机 `csgo_schinese.txt` 读取名称。 |

## 快速开始

1. 使用 HLAE、带 `-insecure` 启动 CS2，然后打开离线 Demo。
2. 下载一个 Release DLL。发布版使用 MSVC 静态运行库（`/MT`），不需要另装 Visual C++ Redistributable、Python、SDK 或 Visual Studio。
3. 在 HLAE 中加载 DLL：

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.zh-CN.dll"
```

英文版：

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.dll"
```

按 `Insert` 显示面板，选择观战玩家 XUID 和外观配置，点击 `Update`。运行诊断会写入 DLL 同目录的 `nSkinzCS2.log`。

## 兼容基线

`v0.4.0` 发布版基于以下版本检查：

- Steam app build ID `24957633`
- CS2 ClientVersion `2000897`、Patch `1.41.7.8`、Source revision `10942762`（2026-08-26）
- `client.dll` SHA-256：`84BCAE9DB379BED5ACB3D81228ABEF86FBD61B6CDB2B3ACCDC5DD0F32E8F89F9`
- HLAE `2.191.1` / AfxHookSource2 `0.40.1`
- `Source2Client002`、`ResourceSystem013`、`VFileSystem017`

CS2 更新可能改变内存布局。DLL 会记录检测到的版本，并在签名缺失或匹配不唯一时拒绝不确定的写入。

<details>
<summary><strong>从源码构建</strong></summary>

要求：Windows 10/11 x64、Visual Studio 2022 + MSVC v143、Windows SDK 10、Direct3D 11 开发库，以及用于目录检查的本地 CS2 安装。

```text
打开 nSkinzCS2.sln
选择 Release | x64       # 英文版
选择 ReleaseZhCn | x64   # 简体中文版
生成解决方案
```

输出文件为 `build/Release/nSkinzCS2.dll`、`build/Release/nSkinzCS2.zh-CN.dll` 和 `build/Release/nSkinzCS2.Tests.exe`。`run-build-and-tests.cmd` 会构建并运行原生回归测试，`run-tests.cmd` 只运行现有测试程序。

</details>

<details>
<summary><strong>仓库结构与架构</strong></summary>

| 路径 | 作用 |
| --- | --- |
| `src/` | 启动、配置、目录解析、运行时、外观辅助逻辑和 ImGui 界面 |
| `tools/` | DIA/RVA 与资源查询辅助程序 |
| `third_party/` | Dear ImGui、MinHook、nlohmann/json 源码 |
| `nSkinzCS2.sln` | DLL 与原生测试项目的 Visual Studio 解决方案 |
| `run-build-and-tests.cmd` | 构建和测试脚本 |

`AppState` 连接启动、UI 和运行时线程；`WeaponAppearance`、`KnifeAppearance`、`GloveAppearance` 将配置决策拆成可在游戏进程外测试的辅助模块；运行时层会先校验接口、签名和实体绑定。

</details>

## 已知限制与路线图

- 刀模替换和部分动画重映射仍属实验功能。
- 如果 Demo 中的刀已经拥有编译后的自定义材质，选择 `原皮（无涂装）` 后第一人称视角仍可能显示旧材质。正数刀涂装已验证；该旧材质清理问题不在 v0.4.0 的已修复声明内。
- 当前优先读取已安装的 CS2 游戏数据，尚未完全改为实时物品 Schema 遍历。

后续计划包括：完善原皮材质清理、扩大刀模/动画覆盖、支持空挂件模块创建，以及跟随 CS2 更新维护兼容性。

## 常见问题

**为什么 `-` 不会清除涂装？** `-（不修改）` 的含义就是保持当前值不变。需要明确去除涂装时请选择 `原皮（无涂装）`。

**修改后没有变化。** 请先加载 Demo，确认观战玩家 XUID 和基础物品正确，再点击 `Update`；同时查看 `nSkinzCS2.log` 中的绑定或兼容性错误。

**为什么刀上仍有旧材质？** 部分 Demo 物品带有编译后的第一人称材质，当前 v0.4.0 原皮路径无法保证全部清除，详见上面的已知限制。

**应该下载哪个 DLL？** 英文界面使用 `nSkinzCS2.dll`，中文界面和中文目录名称使用 `nSkinzCS2.zh-CN.dll`；两者兼容同一套 CS2 基线。

## 贡献与支持

提交 Pull Request 前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。安全问题请参考 [SECURITY.md](SECURITY.md)。反馈问题时请附上准确的 CS2 Build ID、HLAE 版本、DLL 语言、复现步骤和经过脱敏的日志片段。

<a name="support"></a>
## ❤️ 支持项目

如果 nSkinzCS2 对你的离线 Demo 工作流有帮助，欢迎自愿支持。下方是作者提供的无 ID 收款码；赞助完全自愿，不代表提供技术支持合同或功能保证。

<table>
  <tr>
    <td align="center"><strong>微信支付</strong><br><img src="docs/assets/sponsor/wechat-pay.png" alt="微信支付收款码" width="260"></td>
    <td align="center"><strong>支付宝</strong><br><img src="docs/assets/sponsor/alipay.png" alt="支付宝收款码" width="260"></td>
  </tr>
</table>

## 致谢与授权说明

nSkinzCS2 使用 Dear ImGui、MinHook 和 nlohmann/json。重新发布修改后的源码或二进制文件前，请阅读 `third_party/` 下的上游声明。本仓库目前暂不添加根目录许可证。

<p align="center"><a href="https://star-history.com/#Lyan-edit/CS2-nskinz&Date"><img src="https://api.star-history.com/svg?repos=Lyan-edit/CS2-nskinz&type=Date" alt="Star History" width="760"></a></p>

<p align="center"><sub>为可复现的 CS2 离线 Demo 回放而构建。请保持离线并安全使用。</sub></p>
