[![nSkinzCS2 offline demo cosmetic toolkit](./docs/assets/nskinz-banner.svg)](./docs/assets/nskinz-banner.svg)

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
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.3/nSkinzCS2.dll"><img src="https://img.shields.io/badge/Download-English%20DLL-2ea043?style=for-the-badge" alt="Download English DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.3/nSkinzCS2.zh-CN.dll"><img src="https://img.shields.io/badge/下载-中文%20DLL-0969da?style=for-the-badge" alt="Download Chinese DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/tag/v0.4.3"><img src="https://img.shields.io/badge/Release%20notes-v0.4.3-8250df?style=for-the-badge" alt="Release notes"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues/new/choose"><img src="https://img.shields.io/badge/Report%20an%20issue-f85149?style=for-the-badge" alt="Report an issue"></a>
</p>

> **Safety first:** nSkinzCS2 is intended only for offline demo playback started through HLAE with `-insecure`. Never load it on VAC-protected or online servers. The author does not guarantee account safety for use outside this scope.

## What's new in v0.4.3

- Fixed modern/legacy gun mesh selection from installed paint metadata, including M4A4 Full Throttle → The Emperor.
- Fixed first-person gun HUD lookup outside the network-entity cache. An expected but unresolved active HUD now stays pending before any appearance writes.
- Filtered gun finishes by exact weapon/paint compatibility, with the same checks in runtime writes and imported configs. The full catalog is preserved; invalid selections are shown explicitly, not silently changed to vanilla.
- Bound completed gun refreshes to verified carrier/render object generations. Stable bindings, holstering and replicated-field repairs do not invalidate them.
- Retained isolated-panel IME/search fixes, static MSVC `/MT` and embedded UI shaders. Independent knife-model work remains read-only and unfinished.

The user confirmed the Emperor fix in their local demo. **45 native regressions**, **8 targeted checks**, both language builds and isolated loading/rendering checks passed. Catalog coverage is **34 guns / 1,449 valid pairs**, not visual certification of every finish. See the [bilingual v0.4.3 release notes](docs/releases/v0.4.3.md).

<details>
<summary>Retained v0.4.1 fixes</summary>

- Fixed the CS2 crash triggered by IME input and typing in the skin search fields, including English input. The full control panel now runs in an isolated host outside `cs2.exe`, using the system's `rundll32.exe`; no additional application download is required.
- Preserved the full installed-game catalog in the isolated panel, including localized skin names.
- Fixed dynamic weapon finish synchronization and redundant knife material regeneration.
- `Finish only` hides and ignores the knife model selector; model selection is available only in `Finish + model`.
- Removed the blocking mesh-update sleep/retry loop, repeated runtime catalog copies, and synchronous file logging from frame callbacks.

The v0.4.1 validation passed **42 native regression tests**. The mesh-function benchmark improved from 3714 ms to approximately 0.1 ms for 240 updates; this is **not** an in-game FPS measurement or a claim that every demo stutter is resolved. See the [bilingual v0.4.1 release notes](docs/releases/v0.4.1.md) for verification boundaries and known limitations.

</details>

## What is nSkinzCS2?

nSkinzCS2 is a Windows x64 DLL that edits the watched player's cosmetic appearance while a Counter-Strike 2 demo is being viewed. It is loaded with HLAE `mirv_loadlibrary`, reads the installed CS2 catalog, and applies guarded, XUID-targeted overrides through a small Win32 + D3D11 + Dear ImGui control panel.

![nSkinzCS2 control panel](./docs/assets/control-panel.png)

## Feature matrix

| Area | Supported in v0.4.3 | Notes |
| --- | --- | --- |
| Weapons | Compatible finish list, seed, wear, quality, name tag and sticker fields | Exact weapon/paint checks also protect runtime writes and imported configs; modern/legacy meshes follow paint metadata. |
| Knives | Finish, seed and wear; reserved finish + model UI | Finish-only hides model selection. The independent model adapter remains read-only; model/animation replacement is not complete. |
| Gloves | T/CT default glove carriers, model and finish | Default glove definitions `5028` and `5029` are supported on the verified build. |
| Keychains | Modify or clear an existing live module | Creating a keychain module on an item that has none remains experimental. |
| Vanilla / no finish | Explicit `Vanilla (No Finish)` option | This is different from `- (Do not modify)`. A stale first-person knife material may survive on some demo items. |
| Languages | English and Simplified Chinese DLLs | The Chinese build reads names from the installed `csgo_schinese.txt` catalog. |

## Quick start

v0.4.3 embeds precompiled UI shaders and retains `/MT`. No additional runtime installation is needed beyond Windows 10/11 x64, CS2 and HLAE. See [single-DLL loading and diagnostics](docs/portable-loading.md) for `LoadLibrary failed` troubleshooting.

1. Start CS2 through HLAE with `-insecure`, then open an offline demo.
2. Download one of the two Release DLLs. Release binaries use the static MSVC runtime (`/MT`), so no Visual C++ Redistributable, Python, SDK or compiler is required.
3. In HLAE, load the DLL:

```text
mirv_loadlibrary D:\nSkinzCS2.dll
```

For the Simplified Chinese UI:

```text
mirv_loadlibrary D:\nSkinzCS2.zh-CN.dll
```

These examples require the DLL to be in the root of drive D. Use your actual full path. Paths without spaces need no quotes; if there are spaces, use exactly one pair of ASCII quotes, e.g. `mirv_loadlibrary "D:\My Tools\nSkinzCS2.dll"`. Never double-wrap the path or use typographic quotes.

Press `Insert` to show the panel, choose a watched-player XUID and cosmetic profile, then press `Update`. Keep the DLL in a writable folder: the isolated panel and runtime share generated catalog/config sidecars there. Runtime diagnostics are written to `nSkinzCS2.log`; panel diagnostics to `nSkinzCS2.ui.log`.

When upgrading, restart HLAE/CS2 and load **only one** language DLL. Back up your existing config; do not inject the new DLL alongside an already loaded old version. The [Windows x64 ZIP](https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.4.3/nSkinzCS2-v0.4.3-windows-x64.zip) contains both language options and loading instructions.

## Compatibility baseline

`v0.4.3` targets the checked baseline below. The Emperor case is user-confirmed; this is not an exhaustive demo/FPS retest:

- Steam app build ID `25000182`
- CS2 ClientVersion `2000899`, patch `1.41.7.8`, Source revision `10948930` (2026-08-28)
- `client.dll` SHA-256 `B8E2C009763E8CEFB88D89A2BDCF452DB17501553D473DB6DA060DF8E6769EB4`
- HLAE `2.191.1`
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
- If a demo knife already owns a compiled custom material, `Vanilla (No Finish)` can leave that old first-person material visible. Positive knife paint kits are verified; this stale-material reset is not claimed as fixed in v0.4.3.
- Fully live item-schema walking is not yet preferred over installed game data.

Planned work includes a more complete vanilla-material reset, broader knife model/animation coverage, keychain creation for empty modules and continued compatibility updates after CS2 patches.

## FAQ

**Does the input fix disable Chinese or reduce the skin list?** No. The full panel is isolated from CS2's input/text-service path and reads the exported installed-game catalog. Chinese IME and English search remain available. If the list is unexpectedly short, include both logs and the catalog sidecar status in your report.

**Why are fewer finishes listed for a gun?** v0.4.3 shows only that gun's compatible finishes. The full installed catalog is retained. Missing compatibility data blocks the request; an incompatible saved selection is marked unavailable, never silently converted to vanilla.

**A demo still stutters.** v0.4.1 removes confirmed blocking work but does not guarantee a stutter-free demo. Keep CS2 running and include the `Runtime performance` log lines, which separate engine callbacks, cosmetic polling, mesh updates and native refresh costs.

**Why does `-` not remove a finish?** `- (Do not modify)` deliberately leaves the current value untouched. Choose `Vanilla (No Finish)` when you want an explicit no-finish request.

**Nothing changes after editing a profile.** Load the demo first, verify the watched-player XUID and base item, then press `Update`. Check `nSkinzCS2.log` for binding or compatibility failures.

**Why can an old knife texture remain?** Some demo items carry a compiled first-person material that the current v0.4.3 vanilla path cannot always clear; this is the documented limitation above.

**Which build should I use?** Use `nSkinzCS2.dll` for English or `nSkinzCS2.zh-CN.dll` for the Chinese UI and catalog names. Both DLLs target the same CS2 compatibility baseline.

**The repository opens, but its images show broken icons.** Some networks cannot reach GitHub's raw asset host. The README already uses GitHub-relative image paths; as a temporary Windows network workaround, open `C:\Windows\System32\drivers\etc\hosts` in Notepad **as Administrator** and add these lines:

```text
199.232.68.133 raw.githubusercontent.com
199.232.68.133 githubusercontent.com
```

Run `ipconfig /flushdns`, restart the browser, and reload the page. Remove these entries if they stop working or when your normal DNS path is restored. If the repository itself returns `404` in an incognito window, hosts changes cannot repair account-level visibility; contact GitHub Support.

## Contributing and support

Read [CONTRIBUTING.md](CONTRIBUTING.md) before opening a pull request. For security-sensitive issues, use [SECURITY.md](SECURITY.md). Bug reports should include the exact CS2 Build ID, HLAE version, DLL language, reproduction steps and a redacted log excerpt.

<a name="support"></a>
## ❤️ Support the project

If nSkinzCS2 is useful to your offline demo workflow, optional support is appreciated. The QR images below are the author's provided no-ID payment codes; sponsorship is voluntary and does not provide a support contract or feature guarantee.

| **WeChat Pay** | **Alipay** |
| :---: | :---: |
| ![WeChat Pay QR code](./docs/assets/sponsor/wechat-pay.png) | ![Alipay QR code](./docs/assets/sponsor/alipay.png) |

## Acknowledgements

nSkinzCS2 uses Dear ImGui, MinHook and nlohmann/json. Please review the upstream notices under `third_party/` before redistributing modified source or binaries. This repository intentionally has no root license at this time.

<p align="center"><a href="https://star-history.com/#Lyan-edit/CS2-nskinz&Date"><img src="https://api.star-history.com/svg?repos=Lyan-edit/CS2-nskinz&type=Date" alt="Star History" width="760"></a></p>

<p align="center"><sub>Built for reproducible, offline CS2 demo review. Keep it offline and keep it safe.</sub></p>
