# 单 DLL 加载 / Single-DLL loading

## 中文

v0.5.4（2026-09-30）沿用单 DLL 加载与枪械换肤方案，基线为 CS2 2000919 / HLAE 2.192.6，并加入从已安装 CS2 文件验证目标刀动画资源的临时 Demo 重写及稳定的原皮枪械材质初始化。地图模型、刀模、动作和默认载体仍需针对具体 Demo 验收。旧版标签和下载资产保留。详见 [v0.5.4 发布说明](releases/v0.5.4.md)。

### 使用要求

- Windows 10/11 x64、正常工作的 CS2 和 HLAE；仅限 `-insecure` 离线 Demo。
- 不需要额外安装 Visual C++ Redistributable、DirectX SDK / June 2010 运行库、.NET、Python 或 Visual Studio。
- 每次只选择一个语言 DLL，放在可写文件夹。目录、配置和日志由 DLL 自动生成，不是需要单独下载的依赖。
- 当前 v0.6.3 兼容基线：HLAE 2.192.8 / AfxHookSource2 0.41.8；Steam Build 26144732；CS2 Client/Server 2000935 / Patch 1.41.9.2 / Source revision 11162119。已通过当前客户端二进制只读检查、刀／手套严格 PE 证据、完整回归及隔离加载／渲染门禁；不代表地图模型、刀模、动作与默认载体已在所有 Demo 中画面验收。

关闭并重新启动 HLAE/CS2，不要向已有旧 DLL 的进程重复加载新版。把中文版放在 `C:\nSkinzCS2\Lyan_nskinz v0.6.3 zh-CN.dll` 后，在游戏控制台输入：

```text
mirv_loadlibrary "C:\nSkinzCS2\Lyan_nskinz v0.6.3 zh-CN.dll"
```

英文版只需把文件名改为 `Lyan_nskinz v0.6.3 EN.dll`。路径是示例，必须指向已经解压的真实 DLL；不要直接指向 ZIP、GitHub 网页或另一台电脑的路径。ZIP 内文件名含空格，必须保留示例中的一对英文半角双引号。

双语 ZIP 保留上述编译输出名。单独下载使用 `Lyan_nskinz-v0.6.3-EN.dll` / `Lyan_nskinz-v0.6.3-zh-CN.dll`；加载时按磁盘上的实际文件名填写。

没有空格的完整路径可以不加引号。路径有空格时只加一对英文半角双引号，例如 `mirv_loadlibrary "D:\My Tools\Lyan_nskinz v0.6.3 zh-CN.dll"`。不要叠加两对引号，也不要使用中文弯引号。

### 保留的 v0.4.2 加载修复与验证

以下为单 DLL 加载方案的历史验证。v0.5.4 的两份 DLL 必须通过隔离加载门禁；完整回归范围见当前发布说明。

旧版实际已经是 x64 `/MT`，没有直接导入 `MSVCP140.dll` 或 `VCRUNTIME140.dll`，所以不能仅凭 HLAE 的 `LoadLibraryA failed` 文案认定缺少 VC++ 运行库。HLAE 的命令实现使用 `LoadLibraryW`，但没有输出 `GetLastError`。

本次移除 Dear ImGui 面板运行时调用 `D3DCompile` 的路径，改为 DLL 内嵌的预编译 DXBC。**不是延迟加载，也不是让用户另下载编译器 DLL。** 同时不再为 `/MT` DLL 禁用 CRT 所需的线程通知。

两个 DLL 的直接导入均仅为：`KERNEL32.dll`、`USER32.dll`、`SHELL32.dll`、`VERSION.dll`、`IMM32.dll`、`d3d11.dll`。Windows 组件自己的系统依赖（如系统 UCRT）属于操作系统的一部分，不要求额外安装。

后台验证通过：

- 中英文各通过标准 `LoadLibraryW`、System32-only 依赖查找和不兼容编译器测试，共六项。测试目录含中文和空格，清除了开发工具 PATH，只放 DLL 与独立诊断程序，没有目录/配置文件。
- 旧 v0.4.1 正常环境加载成功；在诊断程序目录放置缺少 `D3DCompile` 导出的测试替身时失败，系统错误码为 `127`。新版在同样条件下加载成功。替身仅用于测试，不修改系统文件，不随包分发。
- 实际 ImGui DX11 后端通过无窗口 WARP Feature Level 10.0 渲染、像素读回及三次设备资源重建。
- 中英文文字、中文游戏目录、预编译着色器可重现性和 PE 导入检查通过。
- 未修改的外观逻辑使用现有原生测试程序复跑，42 项通过；新增加载/渲染测试单独构建，不以原生测试代替加载验证。

上述属于本机隔离测试，**不是纯净 Windows 虚拟机测试**。随后用户确认朋友修正加载指令的完整路径/重复引号后加载成功；没有取得其 Windows 错误码，不能把模拟的依赖故障当作其机器已确认的原因，也不能推定所有环境均已覆盖。

### 如果仍显示 failed

先确认实际路径、文件大小与 SHA256。可选诊断工具 `nSkinzCS2.LoadCheck.exe` 本身也是 x64 `/MT`，不需要安装；它只在自己的进程中加载 DLL，不启动游戏、不注入、不打开面板。运行：

```text
nSkinzCS2.LoadCheck.exe "C:\nSkinzCS2\Lyan_nskinz v0.6.3 zh-CN.dll"
```

请反馈完整输出、准确加载指令、Windows 和 HLAE 版本。不要把诊断程序重命名为 `cs2.exe`，也不要用 `regsvr32` 注册此 DLL。

| 错误码 | 含义与检查方向 |
| --- | --- |
| 2 / 3 | 指定文件或路径不存在。 |
| 126 | DLL 或其依赖未找到，不等于一定缺 VC++。 |
| 127 | 依赖的导出函数不存在，可能加载了不兼容的同名 DLL。 |
| 193 | 非兼容的 PE 文件、位数错误或文件损坏。 |
| 5 / 577 | 访问限制或映像校验策略；查看系统安全记录，不要盲目关闭防护。 |
| 1114 | DLL 初始化失败。 |

诊断成功但 HLAE 仍失败时，需要继续调查 CS2 进程内已有模块、路径差异或加载策略；不要继续反复安装运行库。

## English

v0.5.4 (2026-09-30) retains the single-DLL and firearm paths on the CS2 2000919 / HLAE 2.192.6 baseline and adds temporary-demo validation of target knife animation resources from installed CS2 files plus stable stock-firearm material initialization. Map models, knife models/actions and stock carriers still require demo-specific rendered acceptance. Previous tags/assets are preserved. See the [v0.5.4 release notes](releases/v0.5.4.md).

Only Windows 10/11 x64, CS2 and HLAE are required. No separate VC++ redistributable, legacy DirectX runtime/SDK, .NET, Python or compiler installation is needed. Use one language DLL, extracted into a writable folder, with HLAE `-insecure` offline demos only. Restart HLAE/CS2 before upgrading. Generated catalog/config/log files are not additional downloads.

The current v0.6.3 baseline is HLAE 2.192.8 / AfxHookSource2 0.41.8, Steam Build 26144732, CS2 Client/Server 2000935 / Patch 1.41.9.2 / Source revision 11162119. It passes current-client read-only signatures, strict knife/glove PE evidence, the full regression suite and isolated loader/render gates; this is not exhaustive visual certification of every map/knife/glove/demo transition.

```text
mirv_loadlibrary "C:\nSkinzCS2\Lyan_nskinz v0.6.3 EN.dll"
```

Replace the example path with the extracted DLL location; use `Lyan_nskinz v0.6.3 zh-CN.dll` for Chinese. These filenames contain spaces, so keep exactly one pair of ASCII double quotes.

The bilingual ZIP preserves those compiled output names. Standalone downloads use `Lyan_nskinz-v0.6.3-EN.dll` / `Lyan_nskinz-v0.6.3-zh-CN.dll`; load the actual filename on disk.

A full path without spaces needs no quotes. For spaces, use exactly one pair of ASCII double quotes, e.g. `mirv_loadlibrary "D:\My Tools\Lyan_nskinz v0.6.3 EN.dll"`. Never double-wrap the path or use typographic quotes.

The following describes the retained v0.4.2 loading fix and historical checks. Both v0.5.4 DLLs must pass the isolated loader gate; see the current release notes for the complete regression boundary.

v0.4.1 already used x64 `/MT` and did not directly import VC++ runtime DLLs. v0.4.2 additionally replaced runtime `D3DCompile` with embedded, precompiled DXBC, removing `D3DCOMPILER_47.dll` entirely rather than delaying or externally supplying it. Static CRT thread notifications are no longer disabled. Both DLLs now import only Windows components listed above.

Local background tests passed: six new-binary loader cases covering both languages, standard/System32-only search and a deliberately incompatible compiler fixture; two old-binary controls; and actual headless ImGui WARP rendering/pixel readback with three resource recreations. Tests exclude developer PATH entries and config sidecars and cover Unicode/spaced paths. The old binary returned error 127 with the incompatible compiler fixture; both new binaries loaded. System files were not modified and the fixture is not distributed. Text/catalog checks, import auditing and shader reproducibility checks also passed.

These local checks are **not an independent clean-VM test**. The user subsequently confirmed that their friend could load the DLL after correcting the full path/duplicate quotes. No Windows error code was obtained; the simulated dependency failure must not be presented as the affected machine's confirmed cause or as exhaustive environment coverage.

The existing 42-test native regression binary also passed for the unchanged cosmetic runtime. The new loader/renderer checks were built separately and verify the changed paths directly.

For further diagnosis, optionally run the self-contained x64 `nSkinzCS2.LoadCheck.exe` with the DLL path and share its output, DLL SHA256, exact HLAE command and Windows/HLAE versions. It loads in its own process only; it does not launch/inject/control CS2 or open the panel. Never rename it to `cs2.exe` or register this DLL with `regsvr32`. Errors 2/3 indicate path lookup; 126 missing module/dependency; 127 missing export; 193 invalid architecture/image; 5/577 access/image policy; 1114 initialization failure. Do not disable security controls blindly. If standalone loading succeeds but HLAE fails, investigate the game process's modules, path and policy differences.

## Developer checks / 开发验证

Generated shader headers are checked in; normal builds do not need to run FXC. Regenerate only when the shader source changes, using Windows SDK `fxc.exe`:

```text
python tools/build_embedded_dx11_shaders.py --fxc "<Windows SDK>/x64/fxc.exe" --check
python tools/audit_portable_dll.py "build/Release/Lyan_nskinz v0.6.3 EN.dll" "build/Release/Lyan_nskinz v0.6.3 zh-CN.dll"
```

Build `tools/PortabilityTests.vcxproj` with Release/x64 and each `TestKind` of `Loader`, `Renderer`, `CompilerStub`. Then run:

```text
python tools/verify_portable_load.py --english-dll "build/Release/Lyan_nskinz v0.6.3 EN.dll" --chinese-dll "build/Release/Lyan_nskinz v0.6.3 zh-CN.dll" --report build/temp/portable-loader-results.json
```

`tools/verify_release_artifacts.py` now also rejects non-inbox direct imports, managed DLLs, wrong architecture and unaudited delay imports. The compiler fixture is testing-only. Keep it out of release packages.

References: [LoadLibraryW and GetLastError](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryw), [static CRT and thread notifications](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-disablethreadlibrarycalls), [offline shader compilation](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/dx-graphics-hlsl-part1).
