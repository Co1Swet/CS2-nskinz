# Temporary cosmetic demo workflow (updated 2026-10-03)

## Goal

Keep already-painted firearm and glove carriers on the established nSkinzCS2
runtime path, while materializing missing stock/default economy state before a
second demo playback is parsed. Knife model/action state is always serialized
as one transaction. The input demo is read-only. The output is a new file.

## Runtime flow

1. The injected runtime reads the verified
   `ISource2EngineToClient::GetDemoFilePath` slot while the original demo is
   active and atomically publishes `nSkinzCS2.current-demo.txt` beside the DLL.
2. The isolated panel builds rewrite records from enabled firearm, knife and
   glove profiles with a non-zero XUID. Firearm requests use the unchanged
   source/target definition and team `ANY`; knife/glove model requests retain
   their team-specific source definitions.
3. Each knife profile emits source definitions `59` (T) and `42` (CT). Each
   glove profile emits `5028` (T) and `5029` (CT). Target definition, paint,
   seed and normalized wear are written together.
4. The embedded nSkinz rewriter performs a read-only ownership and paint-state
   pass over the
   complete replay. It resolves SteamID -> controller -> pawn -> weapon handles
   using full Source 2 handles (slot plus serial), including entities created
   in later rounds. Common-size demos are read from disk once and reused in
   memory for both phases; files above 768 MiB automatically use 8 MiB buffered
   streaming instead.
5. Every owned firearm and glove is routed independently. Existing paint
   attributes are updated in place, while stock/default carriers receive the
   missing paint fields. A finish-only glove request records
   `preserve_source_definition`; the ownership pass resolves the current
   `m_EconGloves.m_iItemDefinitionIndex` and keeps that model definition while
   materializing the missing paint state.
6. A second pass writes the new replay. Knife definition, model resource
   handle, subclass and paint attributes are written atomically. The target
   animation skeleton is never copied from the source demo: the rewriter first
   verifies `<skeleton>.vnmskel_c` in the installed CS2 `game/csgo` content
   (loose file or `pak01_dir.vpk`), then publishes the standard weapon
   `AnimAssetData` classification row that tells Source 2 to load that installed
   resource. Default-glove EconItemView fields and the changed flag are
   materialized on each selected pawn. Default firearm records receive
   synthetic item identity, quality, paint, seed and normalized wear fields.
   Existing sticker slots replace only their integer-union kit value, preserving
   wear, scale, rotation and CS2 offset/schema fields. A requested missing
   logical slot grows the same dynamic-attribute vector and receives the full
   kit/wear/scale/rotation quartet at the weapon's native preset position. The
   implementation is nSkinz code and does not require an external client,
   Python module or service.
7. The final file is validated for the exact eight-byte `PBDEMS2\0` header and
   placed at `game/csgo/nskinz_temp/nskinz_skin_<source>_<time>.dem`.
8. A local event asks the injected runtime to dispatch the generated
   `playdemo` command on `FRAME_RENDER_PASS`. The panel never calls CS2 engine
   interfaces directly.

## Playback isolation

Generated filenames have the `nskinz_skin_` prefix. During playback of such a
file, the runtime disables only its live knife profiles for the local config
snapshot. This prevents a late runtime write from replacing the atomic knife
definition/model/animation transaction already present in the demo. Firearm
and glove profiles remain enabled for painted-source passthrough items. A
reconstructed glove is recognized read-only by its synthetic identity,
definition, account and paint/seed/wear attributes; when those fields already
match, the live glove write and native refresh callbacks are suppressed. This
prevents the generated stream and the runtime refresh path from alternately
rebuilding the same first-person hands.

## Ownership and distribution boundary

- The rewriter source is under `tools/nskinz-demo-rewriter`; its release binary
  is embedded as DLL `RCDATA` and extracted to a content-addressed temporary
  tool cache, so repeated jobs do not rewrite or rescan the executable.
- The parser/writer is the vendored open-source `source2-demo` 0.5.8 tree with
  nSkinz extensions for missing entity fields and string-table insertion.
- Eligible firearm profiles are present in the request with team `ANY`. Paint
  attributes are reused in place when present and materialized when missing.
  Sticker slots are handled independently from paint state: existing slots
  preserve placement, while missing requested slots append only absent Econ
  attributes and update the vector count.
- The complete MIT/Apache-2.0 component notices and modified source ship with
  the project. There is no external client authentication or runtime download.

## Failure behavior

- Missing CS2 process, missing/invalid source demo, unresolved ownership,
  missing installed target knife skeleton, a source demo without an
  `AnimAssetData` insertion table, or an empty eligible plan fails before
  accepting an output.
- The embedded helper runs outside CS2 with a ten-minute timeout and redirected log;
  failure text is returned to the panel.
- An output is accepted only after required knife/glove records resolve, every
  matched requested sticker slot reparses with exactly the requested kit, and
  the demo header validates. A configured firearm that is absent from the
  replay does not invalidate unrelated successful records. `playdemo` is never
  queued for a failed result.
- The source demo is never deleted, renamed or overwritten.

## Continuous animation-table insertion (validated 2026-09-29)

- A target knife skeleton must be present in the initial `AnimAssetData`
  creation message. Add missing target rows immediately after the last initial
  row and shift every later update index by the number of inserted rows.
- Never replace the source knife row: other players and other knife variants
  may still consume it. Never append at a sparse final index or create empty
  placeholder rows; Source 2 can resolve an empty animation object from that
  layout and crash while dereferencing its skeleton.
- The regression source had continuous initial rows `0..12`. Bayonet was added
  as row `13` at tick `-1`, and the first later HE-grenade row moved from `13`
  to `14` at tick `2259`. All initial rows had both a key and the complete
  38-byte weapon animation metadata value.
- Knife changes remain scoped to handles owned by the selected SteamID, across
  every later entity creation. The former definition-wide `42/59` replacement
  route is intentionally removed because it changed unrelated players.
- Reconstructed gloves also materialize bodygroup choice `0000 = 1` together
  with `m_nEconGlovesChanged = 1` and the complete EconItemView attributes.
  This is required for a default glove carrier to render the requested model.

## Verification

- Release build and the complete native regression suite pass.
- `TestDemoSkinRewritePlan` verifies default T/CT knife and glove definitions,
  `ANY` firearm records, target definition/paint/seed/wear serialization, XUID
  grouping, finish-only exclusion and generated-demo detection.
- Rust tests verify the exact weapon `AnimAssetData` metadata layout and both
  supported installed-resource lookup paths: loose compiled files and the CS2
  VPK directory tree.
- A 254 MB source replay was rewritten and independently parsed back: 35 knife
  entity handles, one glove pawn and the inserted knife action skeleton were
  present with the requested fields. Rendered CS2 acceptance is still required
  with the user's exact replay; the log must show non-zero rewrite counts and
  the queued/dispatched `playdemo` command.
- On that same replay and request, replacing whole-entity field enumeration
  with cached direct access to the controller, pawn and 64 weapon-slot paths
  reduced ownership discovery from 580,909 ms to 1,502 ms and total rewrite
  time from 582,593 ms to 2,514 ms. The before/after output SHA-256 values were
  identical, with the same 1 pawn, 35 knives, 1 glove and 1 animation resource.
- The hybrid routing fixture on that replay resolved 16 default weapon records
  for reconstruction, passed 21 already-painted weapon records through, and
  reconstructed one default glove. It produced a valid `PBDEMS2` output in
  4,640 ms (62 ms input, 3,606 ms discovery, 971 ms rewrite).
- The 78.6 MB default-item regression completed in 7,701 ms (18 ms input,
  2,897 ms discovery, 4,781 ms rewrite), resolved one pawn, 25 owned knife
  handles, one glove and one installed animation resource. Independent parsing
  confirmed initial rows `0..13`, Bayonet row `13`, the shifted HE-grenade row
  `14`, the native Bayonet model/subclass tuple, default glove definition
  `5032`, bodygroup `1` and the glove changed flag.
- A later regression on the same replay found that stock AK-47 entities can
  retain decoded paint child slots while the authoritative dynamic-attribute
  vector count is zero. Paint discovery now limits inspection to the active
  vector length. The corrected rewrite resolved all seven owned stock AK-47
  handles as rebuilds (`weapon_count=7`, passthroughs `0`); independent parsing
  confirmed count `3`, paint definition `6` and paint kit `801` on all seven.
- The rewritten glove timeline remained stable at definition `5032`, attribute
  count `3`, paint `10053`, bodygroup `1` and changed flag `1`. Runtime playback
  now treats that exact generated carrier as complete and performs no second
  live write/refresh; a painted passthrough whose identity or requested finish
  differs still follows the existing live path.
- Both English and Chinese Release DLLs embed the same 1,306,112-byte helper
  (`SHA-256 6e39fe916d0f33d17a4263cf8fe256dbd427fce04a2f116f38f3b120eef11a5d`).
  All six isolated loader cases and the complete native test suite pass.
