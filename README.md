# nSkinzCS2

`nSkinzCS2` is a Windows x64 DLL for Counter-Strike 2 demo playback workflows. It is designed to be injected through HLAE `mirv_loadlibrary`, then attach to CS2 runtime structures, parse installed game data, and apply cosmetic overrides for the watched player during demo playback.

This repository is the standalone `nSkinzCS2` project snapshot from the larger local workspace. It contains the Visual Studio solution, first-party source code, helper tools, vendored dependencies, and a codebase map suitable for publishing on GitHub.

## Snapshot

- Primary target: `nSkinzCS2.dll`
- Platform: Windows x64
- Language: C++20
- UI stack: Win32 + D3D11 + Dear ImGui
- Hooking/runtime support: MinHook
- Config format: JSON (`nSkinzCS2.json`)
- First-party source files: 26
- First-party source lines: about 77,788

Largest first-party translation units:

- `src/Cs2/Runtime.cpp`: 59,347 lines
- `src/Cs2/GlobalResourceSwapper.cpp`: 8,093 lines
- `src/Tests/Main.cpp`: 3,203 lines
- `src/UI/MainWindow.cpp`: 1,715 lines
- `src/Cs2/Catalog.cpp`: 1,577 lines

## Current Feature Scope

Implemented:

- Dedicated Win32 + D3D11 + Dear ImGui tool window
- Versioned JSON config load/save
- XUID-targeted weapon cosmetic profiles
- Separate knife and glove profile pipelines
- Runtime environment detection for CS2, HLAE, interfaces, schema binding, entity-system binding, and hook install state
- Catalog parsing from installed CS2 files for weapons, gloves, paint kits, stickers, qualities, and visual metadata
- Demo-time cosmetic writes for regular weapons
- Knife finish-only support plus guarded `model_and_finish` paths
- Glove finish-only support plus wearable-backed `model_and_finish` paths
- Validation and regression-style native tests for config parsing, catalog parsing, duplicate detection, helper logic, and runtime decision probes

Still incomplete or intentionally limited:

- Knife type rebuild edge cases and broader animation remap coverage
- Preview glove writes
- Agent replacement
- Fully live item-schema walking instead of preferring installed game data

## Build Requirements

- Windows 10/11 x64
- Visual Studio 2022 with the MSVC v143 toolset
- Windows SDK 10
- Direct3D 11 development libraries
- A local CS2 installation for runtime use and catalog-backed behavior checks
- HLAE if you want to load the DLL with `mirv_loadlibrary`

## Build And Test

### Visual Studio

1. Open `nSkinzCS2.sln`.
2. Select `Release | x64`.
3. Build the solution.

Outputs:

- DLL: `build/Release/nSkinzCS2.dll`
- Test runner: `build/Release/nSkinzCS2.Tests.exe`

### Command Scripts

- `run-build-and-tests.cmd`
  Builds the solution with MSBuild, then runs the native test executable.
- `run-tests.cmd`
  Runs `build/Release/nSkinzCS2.Tests.exe` and prints the captured logs.

## Runtime Usage

1. Build `nSkinzCS2.dll` for `x64`.
2. Start a CS2 session that is compatible with your HLAE workflow.
3. Load the DLL:

```text
mirv_loadlibrary "C:\Path\To\nSkinzCS2.dll"
```

4. Press `Insert` to show or hide the tool window.
5. Open a CS2 demo.
6. Configure weapon, knife, and glove profiles.
7. Press `Update` to request a runtime refresh.

Behavior notes:

- Weapon profiles are keyed by watched-player XUID and base item definition.
- Knife profiles support `finish_only` and guarded `model_and_finish`.
- Glove profiles support `finish_only` and wearable-backed `model_and_finish`.
- Legacy glove entries under `profiles` with `base_item = 5028` are ignored at runtime; use `glove_profiles`.

## Repository Layout

```text
nSkinzCS2/
|-- src/                  First-party source code
|-- third_party/          Vendored dependencies
|-- tools/                Helper utilities and reverse-engineering helpers
|-- nSkinzCS2.sln         Visual Studio solution
|-- nSkinzCS2.vcxproj     DLL project
|-- nSkinzCS2.Tests.vcxproj  Native test project
|-- run-build-and-tests.cmd
|-- run-tests.cmd
`-- README.md
```

Generated outputs such as `build/`, `.obj`, `.exe`, `.dll`, `.pdb`, and logs are intentionally excluded from version control.

## Code Map

### Root Project Files

| Path | Purpose |
| --- | --- |
| `nSkinzCS2.sln` | Visual Studio solution containing the DLL and native test targets. |
| `nSkinzCS2.vcxproj` | Main DLL build configuration, include paths, libraries, and post-build copy step. |
| `nSkinzCS2.Tests.vcxproj` | Native test executable build configuration. |
| `run-build-and-tests.cmd` | MSBuild wrapper that builds the solution and then runs tests. |
| `run-tests.cmd` | Standalone test runner wrapper around `build/Release/nSkinzCS2.Tests.exe`. |

### First-Party Source Files

| Path | Lines | Responsibility |
| --- | ---: | --- |
| `src/dllmain.cpp` | 23 | DLL entry point; starts the bootstrap flow when the library is loaded. |
| `src/Bootstrap.cpp` | 539 | Startup orchestration: locate module paths, load config, build catalog, create runtime controller, and launch the UI thread. |
| `src/Core/AppState.hpp` | 270 | Shared application state model containing config, catalog, runtime status, and refresh flags. |
| `src/Core/Config.hpp` | 34 | Public config storage API for load/save/default document and profile lookup helpers. |
| `src/Core/Config.cpp` | 458 | JSON serialization, config migration, legacy knife/glove normalization, and profile lookup logic. |
| `src/Core/Log.hpp` | 19 | Lightweight logging interface. |
| `src/Core/Log.cpp` | 91 | File and debug-output logging helpers for runtime diagnostics. |
| `src/Cs2/Catalog.hpp` | 20 | Catalog builder interface. |
| `src/Cs2/Catalog.cpp` | 1577 | Parses CS2 item and localization data into weapon, glove, paint-kit, sticker, quality, and visual metadata catalogs. |
| `src/Cs2/GlobalResourceSwapper.hpp` | 61 | Public API for the resource-swap hook and associated diagnostics counters. |
| `src/Cs2/GlobalResourceSwapper.cpp` | 8093 | Hooking and deferred-work implementation for resource swap interception and model/material redirection. |
| `src/Cs2/Runtime.hpp` | 489 | Runtime controller declarations, decision helpers, probe types, and test-facing utility surfaces. |
| `src/Cs2/Runtime.cpp` | 59347 | Main runtime engine: process/interface discovery, hook install, schema/entity binding, watched-player tracking, refresh logic, and cosmetic application flow. |
| `src/Runtime/WeaponAppearance.hpp` | 133 | Weapon appearance request, binding, payload, snapshot, rollback, and verification types. |
| `src/Runtime/WeaponAppearance.cpp` | 196 | Pure helper logic for resolving weapon appearance bindings and payload construction. |
| `src/Runtime/KnifeAnimation.hpp` | 46 | Knife animation remap declarations. |
| `src/Runtime/KnifeAnimation.cpp` | 142 | Knife animation remap table and validation helpers. |
| `src/Runtime/KnifeAppearance.hpp` | 196 | Knife apply/request/context/gating/verification types for finish-only and model-and-finish flows. |
| `src/Runtime/KnifeAppearance.cpp` | 405 | Knife profile translation, apply gating, render verification, and signature helpers. |
| `src/Runtime/GloveAppearance.hpp` | 187 | Glove carrier, compatibility, model identity, and verification types. |
| `src/Runtime/GloveAppearance.cpp` | 210 | Glove slot-mode resolution, compatibility checks, and render verification helpers. |
| `src/UI/MainWindow.hpp` | 22 | ImGui tool-window thread wrapper. |
| `src/UI/MainWindow.cpp` | 1715 | Full Win32 + D3D11 + ImGui editor UI for config editing, catalog browsing, runtime status, and manual refresh requests. |
| `src/Tests/Main.cpp` | 3203 | Native regression test runner covering config migration, catalog building, runtime helper decisions, binding probes, and appearance helper logic. |

### Helper Tools

| Path | Lines | Purpose |
| --- | ---: | --- |
| `tools/dia_rva_lookup.cpp` | 244 | DIA SDK-based RVA lookup helper used during reverse-engineering and symbol analysis. |
| `tools/resolve_resource_query.cpp` | 68 | Small helper for resource-query inspection and experimentation. |

### Vendored Dependencies

| Path | Purpose |
| --- | --- |
| `third_party/imgui/` | Dear ImGui core and the Win32 + DX11 backends used by the tool window. |
| `third_party/MinHook/` | MinHook sources used for runtime detours and trampoline management. |
| `third_party/json/json.hpp` | Single-header `nlohmann/json` dependency used for config serialization. |

## Architectural Notes

- `AppState` is the shared synchronization point between bootstrap, runtime, and UI threads.
- `Bootstrap.cpp` wires the system together and prepares the initial catalog/config state.
- `Runtime.cpp` is the main operational core and currently concentrates most engine-facing logic in one very large translation unit.
- `WeaponAppearance`, `KnifeAppearance`, and `GloveAppearance` isolate profile-specific decision logic into smaller helper modules that are also exercised by tests.
- `MainWindow.cpp` is the full operator-facing editor and diagnostics surface.
- `Tests/Main.cpp` intentionally exposes many pure helper/probe entry points from `Runtime.hpp` to keep risky runtime logic regression-testable outside the game process.

## Third-Party And Licensing Notes

This repository vendors source from upstream projects:

- Dear ImGui
- MinHook
- nlohmann/json

Review each upstream project license before redistributing binaries or republishing modified sources.

## Publishing Notes

If you are turning this local project into a public GitHub repository:

- Keep generated files out of version control.
- Commit the solution, project files, `src/`, `third_party/`, `tools/`, and scripts.
- Rebuild locally after changing dependencies or project settings.
- Prefer documenting runtime limitations and intended usage clearly, because the implementation is currently tuned for demo playback and guarded runtime experimentation.
