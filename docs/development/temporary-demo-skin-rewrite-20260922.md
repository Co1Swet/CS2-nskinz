# Temporary knife/glove demo workflow (2026-09-22)

## Goal

Keep firearm finishes on the established nSkinzCS2 runtime path, while making
knife model/action state and default gloves coherent before a second demo
playback is parsed. The input demo is read-only. The output is a new file.

## Runtime flow

1. The injected runtime reads the verified
   `ISource2EngineToClient::GetDemoFilePath` slot while the original demo is
   active and atomically publishes `nSkinzCS2.current-demo.txt` beside the DLL.
2. The isolated panel builds rewrite records only from enabled
   `ModelAndFinish` knife/glove profiles with a non-zero XUID and target model.
3. Each knife profile emits source definitions `59` (T) and `42` (CT). Each
   glove profile emits `5028` (T) and `5029` (CT). Target definition, paint,
   seed and normalized wear are written together.
4. The embedded nSkinz rewriter performs a read-only ownership pass over the
   complete replay. It resolves SteamID -> controller -> pawn -> weapon handles
   using full Source 2 handles (slot plus serial), including entities created
   in later rounds.
5. A second pass writes the new replay. Knife definition, model resource
   handle, subclass, paint attributes and matching `AnimAssetData` skeleton are
   written atomically. Default-glove EconItemView fields and the changed flag
   are materialized on each selected pawn. The implementation is nSkinz code
   and does not load an Insight client, Python module or external service.
6. The final file is validated for the exact eight-byte `PBDEMS2\0` header and
   placed at `game/csgo/nskinz_temp/nskinz_skin_<source>_<time>.dem`.
7. A local event asks the injected runtime to dispatch the generated
   `playdemo` command on `FRAME_RENDER_PASS`. The panel never calls CS2 engine
   interfaces directly.

## Playback isolation

Generated filenames have the `nskinz_skin_` prefix. During playback of such a
file, the runtime disables its live knife and glove profiles for the local
config snapshot. This prevents a late runtime write from replacing the
definition/model/animation transaction already present in the demo. Standard
weapon profiles are not disabled and keep the previous implementation path.

## Ownership and distribution boundary

- The rewriter source is under `tools/nskinz-demo-rewriter`; its release binary
  is embedded as DLL `RCDATA` and extracted only into the per-job temp folder.
- The parser/writer is the vendored open-source `source2-demo` 0.5.8 tree with
  nSkinz extensions for missing entity fields and string-table insertion.
- Firearm profiles are intentionally absent from the request; their established
  live nSkinz path remains unchanged.
- The complete MIT/Apache-2.0 component notices and modified source ship with
  the project. There is no external client authentication or runtime download.

## Failure behavior

- Missing CS2 process, missing/invalid source demo, unresolved ownership,
  unavailable knife resource calibration, or an empty eligible plan fails
  before accepting an output.
- The embedded helper runs outside CS2 with a ten-minute timeout and redirected log;
  failure text is returned to the panel.
- An output is accepted only after the helper reports non-zero counts for every
  requested cosmetic class and the demo header validates. `playdemo` is never
  queued for a failed result.
- The source demo is never deleted, renamed or overwritten.

## Verification

- Release build and the complete native regression suite pass.
- `TestDemoSkinRewritePlan` verifies default T/CT knife and glove definitions,
  target definition/paint/seed/wear serialization, XUID grouping, finish-only
  exclusion and generated-demo detection.
- A 254 MB source replay was rewritten and independently parsed back: 35 knife
  entity handles, one glove pawn and the inserted knife action skeleton were
  present with the requested fields. Rendered CS2 acceptance is still required
  with the user's exact replay; the log must show non-zero rewrite counts and
  the queued/dispatched `playdemo` command.
