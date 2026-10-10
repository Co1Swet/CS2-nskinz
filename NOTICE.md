# Notices and provenance

This release is distributed under the GNU Affero General Public License v3.0 only (`AGPL-3.0-only`). The complete license text is in [`LICENSE`](LICENSE).

## MulNX_CS2-derived map-remap work

The HLAE demo map-remap implementation was adapted from and audited against [`Co1Swet/MulNX_CS2`](https://github.com/Co1Swet/MulNX_CS2), commit `361008b208326d9e7916ff2cd58dd57a0492c400` (2026-09-16), with the CS2 2000918 `CSVCMsg_GameSessionConfiguration` parser signature and `s1_mapname` layout synchronized from upstream commit `f2466e18e26a8fc3daf4ac7945bc45220ba95993` (2026-09-27). MulNX_CS2 is distributed under AGPL-3.0. Copyright in MulNX_CS2 remains with its respective author(s) and contributor(s).

The adapted behavior covers the Demo protobuf map/addon fields, spawn-group and string-table messages, server/session fields, root map KeyValues, failed-model tracking, manifest fallback, missing-vmdl lookup, and the `CUtlString` ABI used by those hooks.

Prominent modified/adapted areas in nSkinzCS2 are:

- `src/Runtime/MapRemap.cpp` and `src/Runtime/MapRemap.hpp`
- `src/Runtime/WorkshopMaps.cpp` and `src/Runtime/WorkshopMaps.hpp`
- map-remap integration in `src/Core/AppState.hpp`, `src/Core/Config.cpp`, `src/Cs2/Runtime.cpp`, and `src/UI/MainWindow.cpp`
- corresponding regression coverage in `src/Tests/Main.cpp`
- `docs/map-remap-mulnx-audit.md` and the map-remap documentation

nSkinzCS2 changes the integration substantially: it does not ship or invoke the MulNX file rewriter; it discovers locally installed Workshop subscriptions, persists a versioned configuration, uses MinHook register-preserving bridges, validates all current-build signatures before atomically enabling the chain, adds guarded access and deferred diagnostics, applies radar background handling, and suppresses stale source-map generated models where safe. The unsafe VPK hard-block/redirect experiment is not shipped because it caused a fatal load failure or a stalled demo load.

The complete Corresponding Source for the released DLLs, including build project files and bundled third-party source, is provided with the release. Modified distributions must keep this notice, the license, upstream notices, and the source-availability obligations of AGPL-3.0.

## Other bundled components

Dear ImGui, MinHook, and nlohmann/json retain their own notices and license terms in [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and `third_party/`. Those component licenses continue to apply to their respective files.

## CS2 firearm preview silhouettes

The compact firearm-preview geometry in `src/UI/WeaponPreviewGeometry.generated.hpp` is generated from the Panorama equipment icon resources in a locally installed copy of Counter-Strike 2. The generator is `tools/generate_weapon_preview_geometry.py`; no network download or third-party icon repository is used. Counter-Strike, CS2, the firearm icon designs, and related marks/assets remain the property of Valve Corporation. The generated silhouettes are used only as an offline relative placement guide; nSkinzCS2 is not affiliated with or endorsed by Valve.
