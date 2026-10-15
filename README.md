[![nSkinzCS2 offline demo cosmetic toolkit](./docs/assets/nskinz-banner.svg)](./docs/assets/nskinz-banner.svg)

<h3 align="center">Offline Counter-Strike 2 cosmetic overrides for HLAE demo playback</h3>

Map-resource diagnostics were assisted by [Source 2 Viewer](https://s2v.app) ([ValveResourceFormat](https://github.com/ValveResourceFormat/ValveResourceFormat)).

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
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.6.1/Lyan_nskinz-v0.6.1-EN.dll"><img src="https://img.shields.io/badge/Download-English%20DLL-2ea043?style=for-the-badge" alt="Download English DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.6.1/Lyan_nskinz-v0.6.1-zh-CN.dll"><img src="https://img.shields.io/badge/下载-中文%20DLL-0969da?style=for-the-badge" alt="Download Chinese DLL"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/releases/tag/v0.6.1"><img src="https://img.shields.io/badge/Release-v0.6.1-8250df?style=for-the-badge" alt="v0.6.1 release notes"></a>
  <a href="https://github.com/Lyan-edit/CS2-nskinz/issues/new/choose"><img src="https://img.shields.io/badge/Report%20an%20issue-f85149?style=for-the-badge" alt="Report an issue"></a>
</p>

> **Safety first:** nSkinzCS2 is intended only for offline demo playback started through HLAE with `-insecure`. Never load it on VAC-protected or online servers. The author does not guarantee account safety for use outside this scope.

## What's new in v0.6.1

- Rebased the guarded knife, stock-glove, weapon-material, animation and resource paths on CS2 `2000932` / Patch `1.41.9.1` (Steam Build `25947352`).
- Preserved five-slot sticker creation, custom placement, sliders, exact weapon preview and throttled live synchronization across the update.
- Reduced frame-side overhead by caching entity classification per generation and immutable catalog-derived UI lists.
- Revalidated the full installed catalog, arbitrary-field Demo rewriting, firearm materials, Workshop radar masking and CS2-native radar compilation.
- Updated the tested HLAE baseline to `2.192.7` / AfxHookSource2 `0.41.7`.

See the [bilingual v0.6.1 release notes](docs/releases/v0.6.1.md) for exact compatibility evidence, validation and limitations.

## What's new in v0.5.8 (previous stable)

- Temporary Demo can now create a requested firearm sticker slot that is missing from the source demo, writing the complete ID/wear/scale/rotation group at the weapon's native preset position.
- Existing stickers still replace only their kit ID and retain the demo's wear, scale, rotation and CS2 free-placement data.
- Sticker and paint fields share one collision-safe Econ attribute allocator, and the generated demo is reparsed to verify every requested slot before playback is queued.
- Free placement is intentionally deferred: v0.5.8 does not create or modify offset X/Y or schema attributes. Live Update remains existing-slot only; missing slots require Temporary Demo.

See the [bilingual v0.5.8 release notes](docs/releases/v0.5.8.md) for the exact scope, usage and validation boundary.

## What's new in v0.5.6 (previous stable)

- Fixed valid current-CS2 demos failing with `serialized entity length count mismatch`. The writer maps arbitrary sparse `serialized_entities` tables to the packet's actual field-bearing commands while preserving full-entry and delete-inclusive layouts.
- Runtime status, schema/entity bindings, application counts, diagnostics and catalog warnings moved into a collapsed-by-default Runtime diagnostics panel.
- Source 2 writer `41/41`, demo rewriter `11/11`, both language builds and a 310 MB end-to-end rewrite/reparse passed on the checked CS2 2000922 baseline.

See the [bilingual v0.5.6 release notes](docs/releases/v0.5.6.md).

## What's new in v0.5.4

- Firearms that already carry a networked finish retain the verified v0.4.5 runtime path. Stock/default firearm carriers are now materialized in the generated temporary demo before playback, then continue through the same live appearance path.
- Gloves that already carry a networked finish retain the guarded live path. Stock T/CT glove carriers are materialized in the generated temporary demo with definition, paint, seed and wear before the second playback is parsed.
- Target knife animation skeletons are no longer expected in the source demo. The rewriter validates the target compiled skeleton in the installed CS2 loose content or `pak01_dir.vpk`, then inserts the standard `AnimAssetData` loader record.
- Knife model/animation listeners survive demo reload and entity-generation adoption. Weapon-slot and HUD consumers are separated to address knives appearing in the pistol position and stale pistol/original-dagger actions.
- Stock AK-47 carriers now resolve the verified CS2 2000919 material generator and run the engine-owned material initialization once per entity generation. The resulting material state is cached instead of being refreshed every frame.
- Guns, knives and gloves support name tags. An empty custom value uses `nSkinz CS2`; a non-empty value is serialized as the selected custom name.
- Added released Workshop map-model replacement: discover installed subscriptions, select a target map, arm it during the first demo load, then apply it while the same demo is loaded a second time. Safe stale source-map generated models are suppressed; compiled/baked geometry remains map-dependent.
- Workshop radar overlays now detect map-specific neutral matte/grid levels automatically. Ancient Night's opaque gray grid is removed without using a destructive global threshold, and recovered Inferno Water edge/corridor pixels inherit neighbouring road colour instead of reappearing as opaque black residue.
- Filenames, panel title and startup log identify `v0.5.4`. This release uses CS2 Steam Build `25588766`, Client/Server `2000919`, Patch `1.41.8.6`, Source revision `11049427`; HLAE `2.192.6`.

Map models/radars, knife models/actions and stock carriers still require rendered acceptance for each demo/map pair. The full native suite and isolated loader gates pass, but automated validation is not a substitute for that visual check. See the [bilingual v0.5.4 release notes](docs/releases/v0.5.4.md).

## What's new in v0.4.5

- Fixed stock firearms failing to apply a selected finish on CS2 builds where Schema exposes the 72-byte attribute layout but no vector mutation callback. Missing paint/seed/wear nodes now use a strictly byte-verified engine-owned append path.
- Removed the duplicate full entity scan seen in the supplied log. The verified CS2 chunk layout now provides direct indexed lookup, and live-ceiling discovery fills the cache in the same pass.
- Repeated Update operations reuse the neutral engine-owned attribute tail instead of growing the vector each time.
- Included the current independent knife-model chain as an **experimental** source/runtime feature. Model, deploy and inspect replacement have progressed, but first-deploy continuity, dropped/teammate-issued knives and every HUD handoff are not certified complete.
- Compiled/ZIP binaries are named `Lyan_nskinz v0.4.5 EN.dll` and `Lyan_nskinz v0.4.5 zh-CN.dll`; both retain static `/MT` and embedded UI shaders. GitHub normalizes spaces in direct attachment names, so the standalone download assets use stable hyphenated aliases.

The supplied v0.4.4 log showed a firearm request ending at `changed_slots=0, verified=0` while cosmetic polling averaged about 31 ms. v0.4.5 directly addresses both causes. The complete native regression suite, current-client read-only signature checks and isolated loading checks pass; rendered acceptance on the affected friend's demo remains a user validation boundary. See the [bilingual v0.4.5 release notes](docs/releases/v0.4.5.md).

<details>
<summary>Retained v0.4.3 fixes</summary>

- Fixed modern/legacy gun mesh selection from installed paint metadata, including M4A4 Full Throttle → The Emperor.
- Fixed first-person gun HUD lookup outside the network-entity cache. An expected but unresolved active HUD now stays pending before any appearance writes.
- Filtered gun finishes by exact weapon/paint compatibility, with the same checks in runtime writes and imported configs. The full catalog is preserved; invalid selections are shown explicitly, not silently changed to vanilla.
- Bound completed gun refreshes to verified carrier/render object generations. Stable bindings, holstering and replicated-field repairs do not invalidate them.
- Retained isolated-panel IME/search fixes, static MSVC `/MT` and embedded UI shaders. Independent knife-model work remains read-only and unfinished.

The user confirmed the Emperor fix in their local demo. **45 native regressions**, **8 targeted checks**, both language builds and isolated loading/rendering checks passed. Catalog coverage is **34 guns / 1,449 valid pairs**, not visual certification of every finish. See the [bilingual v0.4.3 release notes](docs/releases/v0.4.3.md).

</details>

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

| Area | Supported in v0.6.1 | Notes |
| --- | --- | --- |
| Weapons | Compatible finish list, seed, wear, quality, name tag and sticker replacement/creation | Temporary Demo can update stock and already-painted weapons. Existing stickers keep their transform/free placement; a requested missing logical slot is created at the weapon's native preset position. Exact weapon/paint checks still protect runtime writes and configs. |
| Knives | Finish, seed and wear; experimental finish + model replacement | Finish-only hides model selection. Model/deploy/inspect replacement is active, but first-deploy, dropped-knife and HUD handoffs remain experimental. |
| Gloves | T/CT glove carriers, model and finish | Existing paint attributes are overwritten in place; default definitions `5028` and `5029` receive missing economy fields before the live appearance pass. |
| Temporary skin demo | Generate and load a verified second `.dem` containing knife, firearm, glove and sticker changes | Implemented by the embedded open-source nSkinz rewriter; it can grow a firearm's Econ attribute vector for missing sticker slots. The source demo is never overwritten, and the output is parsed again before it is loaded. |
| Workshop maps | Detect installed subscriptions; replace the map and safe source-map generated models on the demo's second load | Released beta feature. It does not rewrite the `.dem` or Workshop files. Geometry baked into the source BSP/VPK may remain when the target map has no safe one-to-one replacement. |
| Keychains | Modify or clear an existing live module | Creating a keychain module on an item that has none remains experimental. |
| Vanilla / no finish | Explicit `Vanilla (No Finish)` option | This is different from `- (Do not modify)`. A stale first-person knife material may survive on some demo items. |
| Languages | English and Simplified Chinese DLLs | The Chinese build reads names from the installed `csgo_schinese.txt` catalog. |

## Quick start

v0.6.1 embeds precompiled UI shaders, the independent demo rewriter, the audited Mirage Water radar mask, and retains `/MT`. No additional runtime installation is needed beyond Windows 10/11 x64, CS2 and HLAE. See [single-DLL loading and diagnostics](docs/portable-loading.md) for `LoadLibrary failed` troubleshooting.

1. Start CS2 through HLAE with `-insecure`, then open an offline demo.
2. Download one of the two Release DLLs. Release binaries use the static MSVC runtime (`/MT`), so no Visual C++ Redistributable, Python, SDK or compiler is required.
3. In HLAE, load the DLL:

```text
mirv_loadlibrary "D:\Lyan_nskinz-v0.6.1-EN.dll"
```

For the Simplified Chinese UI:

```text
mirv_loadlibrary "D:\Lyan_nskinz-v0.6.1-zh-CN.dll"
```

These examples require the DLL to be in the root of drive D. Use your actual full path. Paths without spaces need no quotes; if there are spaces, use exactly one pair of ASCII quotes, e.g. `mirv_loadlibrary "D:\My Tools\nSkinzCS2.dll"`. Never double-wrap the path or use typographic quotes.

Press `Insert` to show the panel, choose a watched-player XUID and cosmetic profile, then press `Update`. Keep the DLL in a writable folder: the isolated panel and runtime share generated catalog/config sidecars there. Runtime diagnostics are written to `nSkinzCS2.log`; panel diagnostics to `nSkinzCS2.ui.log`.

When upgrading, restart HLAE/CS2 and load **only one** language DLL. Back up your existing config; do not inject the new DLL alongside an already loaded old version. The [Windows x64 ZIP](https://github.com/Lyan-edit/CS2-nskinz/releases/download/v0.6.1/Lyan_nskinz-v0.6.1-CS2-2000932-windows-x64.zip) contains both language options, game-version evidence, license/notices and loading instructions.

## Workshop map and model replacement

Subscribe to and fully download the target CS2 Workshop map before starting. The new `Maps` tab detects installed subscriptions from Steam's local Workshop manifest and reads only top-level `maps/<name>.vpk` packages.

1. Load the source demo once through HLAE and load nSkinzCS2.
2. Open `Maps`, click `Rescan subscribed maps`, select the target, enable demo map replacement, and click `Use selected workshop map`.
3. Leave `Source demo map` blank for automatic capture, then click the main `Update` button while the first load is active.
4. Confirm the log contains `Map remap ready` and `Map remap armed`.
5. Run `playdemo` for the same demo again. The replacement is applied while the second load is parsed.

All verified protobuf, KeyValues, manifest and resource hooks must be available for the detected CS2 build; otherwise the feature fails closed without installing a partial chain. This has passed compilation, native regression and local Workshop discovery checks, but each target map still needs rendered in-game acceptance.

See the [MulNX map-remap parity audit](docs/map-remap-mulnx-audit.md) for the hook-by-hook comparison and intentional framework differences.

## Temporary cosmetic demo

This source tree can create a second demo so knife definition, subclass/model and animation data, plus missing default-firearm/default-glove economy fields, exist before CS2 parses the new playback. A target knife skeleton is never copied from the source demo: the helper verifies its compiled `.vnmskel_c` in the current CS2 installation's `game/csgo` loose content or `pak01_dir.vpk`, then inserts the standard `AnimAssetData` row that makes Source 2 load that installed resource. The ownership scan decides the route independently for every firearm and glove: a source item with an existing networked paint value is left untouched for the established live path; a stock/default source item is materialized in the temporary demo. A finish-only glove request preserves the glove definition discovered in the source demo.

1. Load the original demo once, configure the watched-player XUID's firearm, knife and glove profiles, and press `Update` so the runtime captures the active demo path.
2. Open `Temporary Demo` and press `Generate and load temporary demo`. The embedded nSkinz helper scans ownership and source attributes first, then serializes the complete knife transaction, firearm/glove economy fields, and requested sticker changes into a new demo. A missing sticker slot receives a complete ID/wear/scale/rotation record at its native preset position; existing slots keep their recorded free-placement data.
3. When the rewrite succeeds, nSkinzCS2 queues `playdemo` automatically. The result log reports rewritten firearms, knives and gloves plus replaced/created sticker-slot counts.
4. The generated file is stored under `game/csgo/nskinz_temp/` with an `nskinz_skin_` prefix. The original demo is unchanged. `Load generated demo again` replays the completed file without rewriting it.

For an nSkinz-generated demo, only live knife mutation is disabled to preserve the serialized model/action transaction. Firearm and glove profiles remain enabled: already-painted source items use the original live logic, while default items start from the complete economy state created in the temporary demo and then receive the remaining live-only fields. Multiple XUIDs are resolved independently during the ownership scan. See the [implementation note](docs/development/temporary-demo-skin-rewrite-20260922.md) for boundaries and failure behavior.

## Compatibility baseline

The current `v0.6.1` source/local build targets the checked baseline below. Temporary Demo can rewrite stock and already-painted firearm/glove carriers, create missing sticker slots and materialize modern placement fields; map models/radars, knives, actions and cosmetic rendering remain acceptance items for each demo. See the [v0.6.1 release notes](docs/releases/v0.6.1.md) for the exact scope and test boundary.

- Steam app build ID `25947352`
- CS2 Client/Server `2000932`, patch `1.41.9.1`, Source revision `11131657` (2026-10-14 16:57:53)
- `client.dll` SHA-256 `44AF2AA0D2C64F90D273444CB393B8B679094F0B02136DBDB89D4EC82A560008`
- `animationsystem.dll` SHA-256 `CA7697BB758200F35C9B3693820135BADF6BD1A63109E0CD775877920D087F03`
- HLAE `2.192.6`; AfxHookSource2 `0.41.6`, SHA-256 `0E1378B81B5B82974EA8DD5C3E701E924D63C452927B50D64C1580F746D01F8B`
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

Outputs are `build/Release/Lyan_nskinz v0.6.1 EN.dll`, `build/Release/Lyan_nskinz v0.6.1 zh-CN.dll` and `build/Release/nSkinzCS2.Tests.exe`. `run-build-and-tests.cmd` builds and runs the native regression suite; `run-tests.cmd` runs the existing test binary.

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

- Knife model replacement and some animation/HUD handoffs remain experimental, especially the first deploy after reconstruction and dropped or teammate-issued knives.
- Workshop map replacement depends on the current CS2 binary signatures and the selected map's assets. A CS2 update or an incomplete subscription disables or prevents the replacement; visual acceptance remains per map.
- If a demo knife already owns a compiled custom material, `Vanilla (No Finish)` can leave that old first-person material visible. Positive knife paint kits are verified; this stale-material reset is not claimed as fixed in v0.4.5.
- Fully live item-schema walking is not yet preferred over installed game data.

Planned work includes a more complete vanilla-material reset, broader knife model/animation coverage, keychain creation for empty modules and continued compatibility updates after CS2 patches. See the [sticker and keychain checkpoint](docs/development/sticker-keychain-checkpoint-20261010.md) for the completed sticker scope, the keychain entity boundary and the recommended demo-rewrite/runtime hybrid design.

## FAQ

**Does the input fix disable Chinese or reduce the skin list?** No. The full panel is isolated from CS2's input/text-service path and reads the exported installed-game catalog. Chinese IME and English search remain available. If the list is unexpectedly short, include both logs and the catalog sidecar status in your report.

**Why are fewer finishes listed for a gun?** v0.4.5 shows only that gun's compatible finishes. The full installed catalog is retained. Missing compatibility data blocks the request; an incompatible saved selection is marked unavailable, never silently converted to vanilla.

**A demo still stutters.** v0.4.1 removes confirmed blocking work but does not guarantee a stutter-free demo. Keep CS2 running and include the `Runtime performance` log lines, which separate engine callbacks, cosmetic polling, mesh updates and native refresh costs.

**Why does `-` not remove a finish?** `- (Do not modify)` deliberately leaves the current value untouched. Choose `Vanilla (No Finish)` when you want an explicit no-finish request.

**Nothing changes after editing a profile.** Load the demo first, verify the watched-player XUID and base item, then press `Update`. Check `nSkinzCS2.log` for binding or compatibility failures.

**Why can an old knife texture remain?** Some demo items carry a compiled first-person material that the current v0.4.5 vanilla path cannot always clear; this is the documented limitation above.

**Which build should I use?** Download `Lyan_nskinz-v0.6.1-EN.dll` for English or `Lyan_nskinz-v0.6.1-zh-CN.dll` for the Chinese UI and catalog names. The combined ZIP retains the compiled filenames with spaces. Both DLLs target the same CS2 compatibility baseline.

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

nSkinzCS2 v0.6.1 is released under [`AGPL-3.0-only`](LICENSE). Its map-remap implementation is adapted from and audited against [MulNX_CS2](https://github.com/Co1Swet/MulNX_CS2) commit `361008b208326d9e7916ff2cd58dd57a0492c400`, with the current CS2 GameSession compatibility delta synchronized from `f2466e18e26a8fc3daf4ac7945bc45220ba95993`; it does not use MulNX's file rewriter. The complete DLL Corresponding Source is released with the binaries. Preserve [`NOTICE.md`](NOTICE.md), the AGPL license, source availability and the component notices under `third_party/` when redistributing or modifying it.

The temporary skin-demo implementation is nSkinz code built on the vendored open-source `source2-demo` parser/writer. Its MIT/Apache-2.0 notice and source are included under `third_party/source2-demo` and documented in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md).

<p align="center"><a href="https://star-history.com/#Lyan-edit/CS2-nskinz&Date"><img src="https://api.star-history.com/svg?repos=Lyan-edit/CS2-nskinz&type=Date" alt="Star History" width="760"></a></p>

<p align="center"><sub>Built for reproducible, offline CS2 demo review. Keep it offline and keep it safe.</sub></p>
