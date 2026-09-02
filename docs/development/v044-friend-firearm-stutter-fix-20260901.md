# v0.4.4 friend firearm apply/stutter fix checkpoint (2026-09-01)

## Scope

This checkpoint pauses the independent knife-model work and fixes the separate
v0.4.4 regression reported from a friend's machine: pressing **Update** caused
periodic game stalls while a stock firearm did not receive the selected finish.
No live CS2 window was started or controlled while producing this checkpoint.

The paused knife-model source snapshot is preserved at:

- `build/checkpoints/knife-first-deploy-20260901/knife-model-paused-before-v044-firearm-fix.zip`
- SHA-256: `10A46F1533ECE9C21146C52DB15A3EB981499C8A8FE27B74E2D7761E9EDE19F4`

## Evidence from the supplied logs

- Target build: ClientVersion `2000899`, PatchVersion `1.41.7.8`,
  SourceRevision `10948930`, dated 2026-08-28.
- Stock AK-47 finish request reached the runtime as definition `7`, paint kit
  `1171`, but reported `changed_slots=0, verified=0`.
- Schema reflection reported a valid 72-byte `CEconItemAttribute`, but no
  network-vector operation or element size for the attribute list. Consequently
  a stock gun with no paint/seed/wear nodes could not create the missing nodes.
- The runtime poll averaged roughly 31 ms every poll while the original engine
  callback stayed around 0.2 ms. The entity ceiling probe and cache population
  were traversing the entity system twice.

## Implementation

1. Resolve and byte-verify the current client's native
   `CUtlVector<CEconItemAttribute>::AddToTail` implementation. It allocates and
   constructs real engine-owned 0x48-byte attribute nodes.
2. For stock firearms missing paint kit (`6`), seed (`7`) or wear (`8`), append
   only the missing nodes through that native function, validate the returned
   index/vector/readback, and then use the already verified targeted material
   generator.
3. Prefer the Schema mutation callback when available. The native fallback is
   enabled only for the exact reflected offsets and current verified machine
   code; unknown builds fail closed.
4. On restore, neutralize native nodes rather than corrupting/shrinking the
   engine vector without an erase callback. A later Update reuses the exact
   neutral tail, preventing repeated editor updates from growing the vector.
5. Byte-verify the current `GetEntityFromIndex` chunk layout and use a direct
   lookup only after that verification. Entity ceiling discovery now populates
   the cache in the same pass, removing the duplicate full scan.

## Verification

- Release x64 test project builds successfully.
- Full test suite passes, including stock-weapon creation, repeat Update tail
  reuse, synchronization/rollback, catalog compatibility, glove paths, knife
  graph/pair tests and material refresh tests.
- Read-only probes against the installed current `client.dll` and
  `animationsystem.dll` pass:
  `standard_weapon_material_native_evidence=PASS` and
  `knife_graph_installed_evidence=PASS`.
- English and Simplified Chinese DLLs build with static MSVC runtime (`/MT`).
- Both DLLs pass isolated `LoadLibraryW`, UI export, CRT thread callback and
  `FreeLibrary` tests with System32-only dependency search.
- Direct imports are Windows inbox libraries only: `d3d11.dll`, `USER32.dll`,
  `SHELL32.dll`, `VERSION.dll`, `KERNEL32.dll`, and `IMM32.dll`.

Development artifacts:

- `build/v044-firearm-fix-dev/en/nSkinzCS2.dll`
  - SHA-256: `3D0610B753EE56DD1864FA78DED587B8F4A1556132BFCAD14604B6A298932B89`
- `build/v044-firearm-fix-dev/zh/nSkinzCS2.zh-CN.dll`
  - SHA-256: `3EC70472DA748A36F68351483BA8977A907B9BE9F7429489AB1BA00C9207DDAB`

## Remaining validation boundary

The offline tests prove the current binary signatures, vector invariants,
material-call routing, repeated-update behavior and portable loading. They do
not prove rendered gameplay on the friend's exact demo/machine. Validate this
development DLL in an HLAE `-insecure` offline demo and retain both runtime logs
if the selected finish or poll timing still differs.
