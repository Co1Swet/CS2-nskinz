# Map remap parity audit / 地图更换对照审计

Reference / 参考：[`Co1Swet/MulNX_CS2`](https://github.com/Co1Swet/MulNX_CS2), commit `361008b208326d9e7916ff2cd58dd57a0492c400` (2026-09-16). MulNX_CS2 is licensed under AGPL-3.0. The map-remap portions described below are treated as adapted/derived work and are released under `AGPL-3.0-only`; nSkinzCS2 uses its own integration framework and does not import the MulNX file-rewriter pipeline. See [`NOTICE.md`](../NOTICE.md).

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
[MapRemap/Manifest/path]
```

At minimum, the hooks must report installed and the replacement count must rise above zero. Static tests cannot certify rendered output for every Workshop map; the selected map still requires an HLAE second-load acceptance test.
