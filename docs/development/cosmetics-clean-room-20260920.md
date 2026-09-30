# Knife/glove cosmetics behavior observations (2026-09-20)

## Scope and license boundary

This document records the observable Source 2 state required for a complete
knife/glove transaction. The implementation in this tree is an independent
clean-room implementation based on controlled input/output experiments and
publicly observable replay state; it does not copy or link closed components,
private protocols, or third-party executable code.

nSkinz remains a HLAE / `-insecure` / offline-demo-only DLL. It does not patch
the source demo on disk and does not add an online or anti-cheat path.

## Reference behavior confirmed by differential demos

The reference application rewrites owned cosmetic state in a second demo file.
Its request items contain:

- source `item_id64` and `definition_index`;
- paint kit, pattern seed and wear;
- optional replacement definition for knife/glove model changes;
- explicit `T`, `CT` or `ANY` team scope.

An empty/default slot uses `item_id64 = 0`. Observed source definitions are T
glove 5028, CT glove 5029, T knife 59 and CT knife 42.

### Default glove materialisation

For an empty T glove slot changed to definition 5034 / paint 10033, the output
adds a complete `CCSPlayerPawn::m_EconGloves` item view:

- target definition, paint, seed and wear attributes;
- initialized/account fields;
- `m_nEconGlovesChanged = 1`;
- a valid synthetic item identity.

The observed high word is `0xC5210000 | target_definition`. The low word is a
stable non-reserved value. No separate glove model-resource rule was present in
the captured request: Source 2 materialises the visual from the complete Pawn
econ view and its changed counter.

### Default knife materialisation

For T default knife 59 changed to karambit 507, the output contains one coherent
target state before entity consumption:

- target item definition and full finish attributes;
- synthetic high word `0xC52101FB` (507);
- target `m_nSubclassID` token;
- target model resource handle and mesh/body state;
- an `AnimAssetData` entry for the target VNMSKEL resource.

For a real inventory knife, the observed output preserves the original item ID
while changing definition/finish/model identity.

This explains the historical nSkinz symptom where a target knife model used the
source knife or pistol placement/inspect animation: publishing only the model
after Source 2 built the original subclass/animation graph creates a mixed
state. Definition, subclass/VData, model and animation serialization context
must become visible as one transaction before the native weapon update.

## nSkinz clean-room mapping

### Gloves

`BuildSyntheticGloveEconItemIdentity` independently derives a stable low word
with FNV-1a and uses the observed high-word namespace. Both Pawn `m_EconGloves`
and the real-`CEconWearable` fallback now use and verify this identity. The
existing paint/seed/wear attribute construction and Pawn reapply/changed fields
remain the native visual trigger.

### Knives

`ResolveKnifeEconItemHigh` preserves a real inventory high word and materialises
an invalid/default high word as `0xC5210000 | target_definition`. The holstered
pre-arm and active `BeforeWeapon` handoff keep this identity coherent with the
target definition and subclass transaction. The subsequent finish refresh
reasserts it after the generic cosmetic path runs.

The target animation skeleton is handled by the existing narrowly scoped local
serialization bridge when the target VNMSKEL is absent from the recorded
`AnimAssetData`. The bridge leaves recorded contexts untouched.

## Remaining runtime difference

The reference application edits packet/entity fields before CS2 decodes the
demo. nSkinz discovers the live entity at `FRAME_RENDER_PASS` and then catches
the verified native weapon-update callback. Therefore a demo that begins with
the watched knife already active can still have one source-built graph before
the first matching callback is armed.

Do not guess an unverified FrameStage number or mutate the active entity from a
generic poll: previous mixed model/pistol graphs and crashes came from exactly
that timing error. The remaining clean runtime solution is an earlier,
build-verified demo/entity-field callback that publishes definition,
subclass/VData and animation context before the first native knife update.

## Required live evidence

For a successful default-glove transaction, the log must show a verified Pawn
glove slot with `item_id=0xC521....:...`, target definition, paint/seed/wear and
a completed native world/HUD visual refresh.

For a successful knife transaction, the log must show:

- holstered pre-arm or active `BeforeWeapon` transaction;
- target definition and `item_id_high=0xC521....` for a default knife;
- target subclass token and non-null target VData;
- serialization bridge armed before the definition change;
- world/HUD target pair verification;
- no fallback to the source knife animation graph.
