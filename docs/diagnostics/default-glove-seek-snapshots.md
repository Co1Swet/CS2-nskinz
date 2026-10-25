# Default T/CT glove reconstruction: seek-snapshot failure

Confirmed 2026-10-25 with `g161-20261023231958556851203_de_mirage.dem`.

## Evidence

The target Pawn (handle `3326094`) is created at tick 1335 with team 0, joins CT at tick 1441 and switches to T at tick 74745. The failed nSkinz output contains the requested definition 5034 and paint 10033 during linear parsing after tick 1441. The injected runtime nevertheless reads definition 0 after loading/seeking.

Independent seek reads explain the discrepancy:

| Read mode / position | Failed nSkinz | Insight reference | Repaired nSkinz |
| --- | --- | --- | --- |
| Linear, tick 1441 | definition 5034, quality 3, 3 attributes | definition 5030, quality 3, 3 attributes | definition 5034, quality 3, 3 attributes |
| Seek to tick 40000 (CT) | definition 0, quality -1, 0 attributes | definition 5030, quality 3, 3 attributes | definition 5034, quality 3, 3 attributes |
| Seek to tick 70000 (CT) | definition 0, quality -1, 0 attributes | definition 5030, quality 3, 3 attributes | definition 5034, quality 3, 3 attributes |
| Seek to tick 100000 (T) | definition 0, quality -1, 0 attributes | default glove, not requested for this team | definition 5034, quality 3, 3 attributes |

The requested cosmetics differ: nSkinz was configured for Specialist Gloves / Crimson Kimono on both teams; Insight was configured for Sport Gloves / Pandora's Box on CT. These model/paint differences and the T-side scope are intentional. The defect is that the failed nSkinz checkpoints omit the requested glove entirely. Both outputs parse successfully as Demo containers.

## Root cause

`CosmeticRewriter` previously remembered that a Pawn/team had already received the complete glove subtree. That allowed late `m_iTeamNum` updates to materialize a default glove, but also deduplicated subsequent `Created` records for the same handle and team. Those records include independent `CDemoFullPacket` snapshots used when seeking or starting from a later position. Their baseline stayed at definition 0.

The old verifier accumulated sets of values seen during linear playback and accepted an output as long as the desired values occurred somewhere. A normal sequential Parser deliberately skips later full packets, so it never checked the actual seek snapshots. Successful verification therefore did not establish that seek playback would load the same glove.

## Repair

Every `Created` record starts a new materialization epoch, even when its handle and team match a previously processed record. Team-0 creation remains deferred until a real T/CT rule resolves. Ordinary updates within an epoch do not repeatedly append the complete subtree.

The output is additionally read by a non-mutating `DemoWriter` field observer. This path decodes all entity records, including full packets skipped by a linear Parser. It compares every requested glove field in each active-team state, requires coverage of every planned Pawn/team rule and reports the exact tick, event, handle and mismatching field. The generated response and UI log expose `verified_glove_record_count` / `verified_glove_created_count`.

The runtime classifies the carrier before live mutation. Definition 0, stock T/CT definitions 5028/5029, unreadable carriers and all generated-Demo glove records are read-only. A mismatch requires another Temporary Demo. The runtime does not write their Econ fields, schedule glove refresh, or create a Wearable. The low-level Pawn write and native callback entry also guard generated/default carriers.

## Regression validation

- The new verifier rejects the actual previous output at tick 3841, `Created`, definition 0 instead of 5034.
- Rebuilding the original 215,167,603-byte Demo verifies 97,791 target records, including 29 independent `Created` records.
- A second rebuild deliberately uses different T/CT models, paints, seeds, wear and names. Snapshot reads retain CT 5030 / 10037 / seed 321 / wear 0.07 and T 5034 / 10033 / seed 1000 / minimum wear.
- Native policy tests cover default 0/5028/5029, unknown carriers, generated special-glove mismatches and the established non-generated special-glove path.

Local source, logs, request/response JSON and seek evidence are retained in `build/glove-g161-20261025/`. The private Demo itself is not included in source control.

To run the optional real-Demo regression, set `NSKINZ_GLOVE_REGRESSION_REQUEST` to a request JSON whose output is a disposable workspace file, and `NSKINZ_GLOVE_REJECT_DEMO` to the known failing output. Run:

```powershell
cargo test --release --manifest-path tools/nskinz-demo-rewriter/Cargo.toml --bin nskinz-demo-rewriter real_demo_rejects_missing_seek_snapshots_and_rebuilds_all_epochs -- --ignored --nocapture
```

Use `inspect_cosmetics <input.dem> <output.json> <tick> [<tick> ...]` for independent seek readback. Final rendered glove appearance remains an in-game acceptance check; the tests above establish the serialized state and runtime routing.

## Cross-Demo validation

The optional `different_demos_rebuild_default_t_ct_gloves_and_verify_random_access` test reads independent source recordings from `NSKINZ_DEFAULT_GLOVE_MATRIX`. Its JSON format is an array of objects with `input_dem` and `output_dem` paths. All output paths must be disposable files outside the original source locations.

The test discovers actual player/Pawn associations in each source, selects players whose source gloves are default throughout that recording, and requests distinct T and CT replacements. It verifies the complete output and reads six positions in mixed forward/backward order through the independent Parser seek path. Each source must exercise both T and CT. No player, Pawn handle, recording name, field index or field count is built into the production fix.

The 2026-10-25 run passed all four independent recordings:

| Source recording | Default-glove players | Verified target records | Independent Created records | Independent seek readbacks | Teams |
| --- | ---: | ---: | ---: | ---: | --- |
| `9212433153368854924_0.dem` | 7 | 616,406 | 238 | 42 | T and CT |
| `1-5efb2785-4490-4f16-8524-023f837594c0-1-1.dem` | 5 | 361,249 | 140 | 30 | T and CT |
| `1-f27266f3-9574-47d3-a2af-1c2c63ae1a1c-1-1.dem` | 6 | 419,911 | 150 | 36 | T and CT |
| `g161-20261023231958556851203_de_mirage.dem` | 6 | 496,945 | 174 | 24 | T and CT |
| Total (player/recording combinations) | 24 | 1,894,511 | 702 | 132 | Both |

```powershell
cargo test --release --manifest-path tools/nskinz-demo-rewriter/Cargo.toml --bin nskinz-demo-rewriter different_demos_rebuild_default_t_ct_gloves_and_verify_random_access -- --ignored --nocapture
```
