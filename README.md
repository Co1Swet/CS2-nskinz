<p align="center"><img src="docs/assets/nskinz-banner.svg" alt="nSkinzCS2 offline demo cosmetic toolkit" width="900"></p>

<h3 align="center">Offline Counter-Strike 2 cosmetic overrides for HLAE demo playback</h3>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/blob/main/README.md"><img src="https://img.shields.io/badge/English-README-1f6feb?style=flat-square" alt="English README"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/blob/main/README.zh-CN.md"><img src="https://img.shields.io/badge/简体中文-README-555?style=flat-square" alt="简体中文 README"></a>
</p>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/latest"><img src="https://img.shields.io/github/v/release/Lyan-edit/CS2-nskinz?style=flat-square&label=release" alt="Latest release"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases"><img src="https://img.shields.io/github/downloads/Lyan-edit/CS2-nskinz/total?style=flat-square&label=downloads" alt="Total downloads"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz"><img src="https://img.shields.io/github/stars/Lyan-edit/CS2-nskinz?style=flat-square" alt="Stars"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/network/members"><img src="https://img.shields.io/github/forks/Lyan-edit/CS2-nskinz?style=flat-square" alt="Forks"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues"><img src="https://img.shields.io/github/issues/Lyan-edit/CS2-nskinz?style=flat-square" alt="Open issues"></a>
  <img src="https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square" alt="C++20">
  <img src="https://img.shields.io/badge/platform-Windows%20x64-0078D6?style=flat-square" alt="Windows x64">
  <img src="https://img.shields.io/badge/HLAE-offline%20demo-6f42c1?style=flat-square" alt="HLAE offline demo">
</p>

<p align="center">
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.0/nSkinzCS2.dll"><img src="https://img.shields.io/badge/Download-English%20DLL-2ea043?style=for-the-badge" alt="Download English DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.0/nSkinzCS2.zh-CN.dll"><img src="https://img.shields.io/badge/下载-中文%20DLL-0969da?style=for-the-badge" alt="Download Chinese DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/tag/v0.4.0"><img src="https://img.shields.io/badge/Release%20notes-v0.4.0-8250df?style=for-the-badge" alt="Release notes"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues/new/choose"><img src="https://img.shields.io/badge/Report%20an%20issue-f85149?style=for-the-badge" alt="Report an issue"></a>
</p>

> **Safety first:** nSkinzCS2 is intended only for offline demo playback started through HLAE with `-insecure`. Never load it on VAC-protected or online servers. The author does not guarantee account safety for use outside this scope.

## What is nSkinzCS2?

nSkinzCS2 is a Windows x64 DLL that edits the watched player's cosmetic appearance while a Counter-Strike 2 demo is being viewed. It is loaded with HLAE `mirv_loadlibrary`, reads the installed CS2 catalog, and applies guarded, XUID-targeted overrides through a small Win32 + D3D11 + Dear ImGui control panel.

<p align="center"><img src="docs/assets/control-panel.png" alt="nSkinzCS2 control panel" width="900"></p>

## Feature matrix

| Area | Supported in v0.4.0 | Notes |
| --- | --- | --- |
| Weapons | Finish, seed, wear, quality, name tag and sticker fields | Profiles are isolated by watched-player XUID and base item definition. |
| Knives | Finish-only replacement, seed and wear | The live knife model is preserved. Positive paint kits are verified on Skeleton, M9, Butterfly and Karambit entities. |
| Gloves | T/CT default glove carriers, model and finish | Default glove definitions `5028` and `5029` are supported on the verified build. |
| Keychains | Modify or clear an existing live module | Creating a keychain module on an item that has none remains experimental. |
| Vanilla / no finish | Explicit `Vanilla (No Finish)` option | This is different from `- (Do not modify)`. A stale first-person knife material may survive on some demo items. |
| Languages | English and Simplified Chinese DLLs | The Chinese build reads names from the installed `csgo_schinese.txt` catalog. |

## Quick start

1. Start CS2 through HLAE with `-insecure`, then open an offline demo.
2. Download one of the two Release DLLs. Release binaries use the static MSVC runtime (`/MT`), so no Visual C++ Redistributable, Python, SDK or compiler is required.
3. In HLAE, load the DLL:

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.dll"
```

For the Simplified Chinese UI:

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.zh-CN.dll"
```

Press `Insert` to show the panel, choose a watched-player XUID and cosmetic profile, then press `Update`. Diagnostics are written beside the DLL as `nSkinzCS2.log`.

## Compatibility baseline

The `v0.4.0` binaries were checked against:

- Steam app build ID `24957633`
- CS2 ClientVersion `2000897`, patch `1.41.7.8`, Source revision `10942762` (2026-08-26)
- `client.dll` SHA-256 `84BCAE9DB379BED5ACB3D81228ABEF86FBD61B6CDB2B3ACCDC5DD0F32E8F89F9`
- HLAE `2.191.1` / AfxHookSource2 `0.40.1`
- `Source2Client002`, `ResourceSystem013` and `VFileSystem017`

CS2 updates can change memory layouts. The DLL logs its detected identity and fails closed when required signatures are missing or ambiguous.

<details>
<summary><strong>Build from source</strong></summary>

Requirements: Windows 10/11 x64, Visual Studio 2022 with MSVC v143, Windows SDK 10, Direct3D 11 libraries, and a local CS2 installation for catalog-backed checks.

```text
Open nSkinzCS2.sln
Select Release | x64       # English
Select ReleaseZhCn | x64   # Simplified Chinese
Build the solution
```

Outputs are `build/Release/nSkinzCS2.dll`, `build/Release/nSkinzCS2.zh-CN.dll` and `build/Release/nSkinzCS2.Tests.exe`. `run-build-and-tests.cmd` builds and runs the native regression suite; `run-tests.cmd` runs the existing test binary.

</details>

<details>
<summary><strong>Repository and architecture</strong></summary>

| Path | Purpose |
| --- | --- |
| `src/` | Bootstrap, config, catalog, runtime, appearance helpers and ImGui UI |
| `tools/` | DIA/RVA and resource-query helper programs |
| `third_party/` | Dear ImGui, MinHook and nlohmann/json sources |
| `nSkinzCS2.sln` | Visual Studio solution for the DLL and native tests |
| `run-build-and-tests.cmd` | Build and test wrapper |

`AppState` connects the bootstrap, UI and runtime threads. `WeaponAppearance`, `KnifeAppearance` and `GloveAppearance` keep profile decisions testable outside the game process, while the runtime layer validates interfaces, signatures and entity bindings before writing.

</details>

## Known limitations and roadmap

- Knife model replacement and a few animation-remap cases are still experimental.
- If a demo knife already owns a compiled custom material, `Vanilla (No Finish)` can leave that old first-person material visible. Positive knife paint kits are verified; this stale-material reset is not claimed as fixed in v0.4.0.
- Fully live item-schema walking is not yet preferred over installed game data.

Planned work includes a more complete vanilla-material reset, broader knife model/animation coverage, keychain creation for empty modules and continued compatibility updates after CS2 patches.

## FAQ

**Why does `-` not remove a finish?** `- (Do not modify)` deliberately leaves the current value untouched. Choose `Vanilla (No Finish)` when you want an explicit no-finish request.

**Nothing changes after editing a profile.** Load the demo first, verify the watched-player XUID and base item, then press `Update`. Check `nSkinzCS2.log` for binding or compatibility failures.

**Why can an old knife texture remain?** Some demo items carry a compiled first-person material that the current v0.4.0 vanilla path cannot always clear; this is the documented limitation above.

**Which build should I use?** Use `nSkinzCS2.dll` for English or `nSkinzCS2.zh-CN.dll` for the Chinese UI and catalog names. Both DLLs target the same CS2 compatibility baseline.

## Contributing and support

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. For security-sensitive issues, use [SECURITY.md](SECURITY.md). Bug reports should include the exact CS2 Build ID, HLAE version, DLL language, reproduction steps and a redacted log excerpt.

<a name="support"></a>
## ❤️ Support the project

If nSkinzCS2 is useful to your offline demo workflow, optional support is appreciated. The QR images below are the author's provided no-ID payment codes; sponsorship is voluntary and does not provide a support contract or feature guarantee.

<table>
  <tr>
    <td align="center"><strong>WeChat Pay</strong><br><img src="docs/assets/sponsor/wechat-pay.png" alt="WeChat Pay QR code" width="260"></td>
    <td align="center"><strong>Alipay</strong><br><img src="docs/assets/sponsor/alipay.png" alt="Alipay QR code" width="260"></td>
  </tr>
</table>

## Acknowledgements

nSkinzCS2 uses Dear ImGui, MinHook and nlohmann/json. Please review the upstream notices under `third_party/` before redistributing modified source or binaries. This repository intentionally has no root license at this time.

<p align="center"><a href="https://star-history.com/#Lyan-edit/CS2-nskinz&Date"><img src="https://api.star-history.com/svg?repos=Lyan-edit/CS2-nskinz&type=Date" alt="Star History" width="760"></a></p>

<p align="center"><sub>Built for reproducible, offline CS2 demo review. Keep it offline and keep it safe.</sub></p>
