# Map remap parity audit / 地图更换对照审计

Reference / 参考：[`Co1Swet/MulNX_CS2`](https://github.com/Co1Swet/MulNX_CS2), full behavior audit at commit `361008b208326d9e7916ff2cd58dd57a0492c400` (2026-09-16), plus the CS2 2000918 GameSession compatibility delta from commit `f2466e18e26a8fc3daf4ac7945bc45220ba95993` (2026-09-27). MulNX_CS2 is licensed under AGPL-3.0. The map-remap portions described below are treated as adapted/derived work and are released under `AGPL-3.0-only`; nSkinzCS2 uses its own integration framework and does not import the MulNX file-rewriter pipeline. See [`NOTICE.md`](../NOTICE.md).

The 2026-09-27 delta masks the relative branch displacements in the `CSVCMsg_GameSessionConfiguration` shared-string parser signature and moves field 11 (`s1_mapname`, has-bit 2) from the old `+0x28` slot to the current `+0x40` slot. The upstream pattern uniquely matches RVA `0x17D153` in the verified CS2 2000918 `engine2.dll`.

## Runtime parity / 运行时对齐

| Component | MulNX behavior | nSkinzCS2 behavior after audit | Result |
| --- | --- | --- | --- |
| DemoFileHeader map | RSI message, has-bit `0x8`, string slot `+0x30` | Same signature, register, bit and slot | Aligned |
| DemoFileHeader addon | has-bit `0x20`, string slot `+0x40`; independent of raw-map match | Same; the earlier header-match dependency was removed | Aligned |
| SpawnGroup | RSI message, successful RAX, has-bit `0x1`, slot `+0x18` | Same | Aligned |
| ClearAllStringTables | RSI message, slot `+0x18` | Same | Aligned |
| ServerInfo | RSI message, slot `+0x20` | Same | Aligned |
| GameSessionConfiguration | RSI message, has-bit `0x4`, slot `+0x28` | Same | Aligned |
| Root map KeyValues | RBP is `KeyValues`; set `map` and `levelname` to target without waiting for a header-match flag | Same signature/export and trigger behavior; the earlier ordering gate was removed | Aligned |
| Failed model tracking | At the resourcesystem log point, RAX is a path; record `.vmdl_c` after removing `_c` | Same | Aligned |
| Manifest fallback | At the engine2 manifest point, R8 is the path; replace a recorded missing model with `models/tools/bullet_hit_marker.vmdl` | Same and always active after hook installation | Aligned |
| Missing vmdl request | For type `0x6C646D76` and nonzero hash, call FindByHash at resource-system `+0x2BE0`; return null when absent | Same and no longer dependent on DemoHeader ordering | Aligned |
| CUtlString | 32-byte object, SSO capacity `<= 0xF`, tagged pointer low two bits, engine Assign function | Same layout and Assign signature, with additional guarded reads | Aligned |

## Intentional differences / 有意保留的差异

- MulNX stores three atomic shared strings and requires manual raw map/addon/map input. nSkinzCS2 persists a versioned config, discovers installed Steam Workshop maps, validates the numeric Workshop ID, and fills the raw map from the active runtime level.
- MulNX installs separate modules through its LoadLibrary message bus and RegContext hook framework. nSkinzCS2 installs the equivalent chain after `Update`, before the second demo load, using MinHook plus register-preserving bridges.
- nSkinzCS2 requires every current-build signature/export to be present and unique, then enables all hooks atomically. MulNX attaches the modules independently. The nSkinz approach deliberately fails closed instead of leaving a partial remap chain.
- nSkinzCS2 protects pointer/string access with SEH, bounds copied paths, clears resource-error state on every new map configuration, and removes the hooks on shutdown.
- nSkinzCS2 adds replacement counters and deferred stage logs. These diagnostics do not write to disk synchronously from the hook callback.
- Neither implementation needs to rewrite the `.dem` for this runtime map-remap path. nSkinzCS2 intentionally does not include any MulNX file-rewriter pipeline.

## Workshop radar mask controls / 工坊雷达遮罩调节

Workshop authors bake different black/grid levels into their radar textures.
A single global RGB cutoff removed dark Nuke corridors on some maps while
leaving grid fragments on others. The map page therefore exposes three
per-config controls. Releasing a slider regenerates and publishes a native
VTEX with a content-derived cache key; the global `Update` button remains an
equivalent explicit path.

- `Black-background tolerance / 黑底容差` (default `24`): raise it in steps of
  2-4 when black grid fragments remain; lower it when dark roads disappear.
- `Dark-road recovery radius / 暗色道路恢复半径` (default `4 px`, maximum `6 px`):
  this restores dark pixels and does not remove the background. Raise it only
  when a dark corridor is missing. Values above four require multi-axis
  enclosure so a long straight editor-grid line cannot be bridged back into
  the foreground. If black stripes return, lower this value and raise the
  black-background tolerance instead.
- `Classified background opacity / 已识别黑底的不透明度` (default `0%`): `0%`
  is fully transparent. This changes only pixels classified as background and
  is independent of player markers.

The external UI decodes and compiles the radar; the injected CS2 runtime only
validates and mounts the finished native resource. No Workshop, CS2, or demo
file is modified. Because Panorama can retain an already loaded texture,
release the slider (or press `Update`) and run the same `playdemo` again after
tuning the controls.

## Dynamic-model lighting compatibility / 动态模型照明兼容

Demo playback can retain source-map resource references after the world and
spawn-group names have been changed to the Workshop target. If the stale
`default_ents.vents_c` cannot be resolved from the newly mounted map package,
the target's `env_combined_light_probe_volume` entities are never created.
Static geometry can still look correct while both teams' player models render
nearly black.

nSkinzCS2 redirects only these standardized runtime-data paths from
`maps/<source>/` to `maps/<target>/` during the second demo load:

- `entities/default_ents.vents[_c]`;
- the two `lightmaps/env_light_probe_volume_atlas*.vtex[_c]` resources;
- `cubemaps/env_cubemap_array.vtex[_c]`.

Numeric probe-octree names are not portable between compiled maps. Comparing
the decompiled entity lumps showed that stock Nuke and Rainy Nuke have 92
probe volumes with identical origins, dimensions and atlas coordinates, but
their handshake IDs were regenerated in reverse order (`1372856349..443` to
`303904010..916`). That verified pair set is mapped explicitly. Every other
map fails closed instead of constructing a target octree path that does not
exist. `[MapRemap/LightingAudit]` additionally reports whether live combined
light-probe entities are present after the second load.

Rainy Nuke's target probe atlas renders demo-driven player models too dark in
the remapped session. Because that pair is geometry- and atlas-coordinate
identical, target atlas requests are routed back to stock `de_nuke` probe
samples. Static Workshop geometry, weather, post-processing, cubemaps and
radar remain target-owned. No other map pair receives this source-lighting
fallback without the same structural verification.

Generated entity/world-node `.vmdl` resources remain on the separate,
fail-closed stale-geometry suppression path. Ordinary player, weapon, prop and
material resources are not matched. The runtime log reports successful
routes as `[MapRemap/RuntimeData/Manifest]`,
`[MapRemap/RuntimeData/Open]`, or `[MapRemap/RuntimeData/ReadFile]`.

## Expected second-load evidence / 第二次加载应出现的证据

After selecting a Workshop map and pressing `Update`, the runtime log must first contain `Map remap ready` and `Map remap armed`. During the second `playdemo`, successful stages are logged with prefixes such as:

```text
[MapRemap/DemoHeader/map_name]
[MapRemap/DemoHeader/addons]
[MapRemap/SpawnGroup/name]
[MapRemap/ClearAllStringTables/mapname]
[MapRemap/ServerInfo/map_name]
[MapRemap/GameSession/s1_mapname]
[MapRemap/KeyValues/map+levelname]
[MapRemap/RuntimeData/Manifest]
[MapRemap/Manifest/path]
```

At minimum, the hooks must report installed and the replacement count must rise above zero. Static tests cannot certify rendered output for every Workshop map; the selected map still requires an HLAE second-load acceptance test.
