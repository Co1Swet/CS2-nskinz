# Knife/glove clean-room checkpoint (2026-09-21)

This file freezes the cosmetics reverse-engineering state before the Workshop
radar controls were changed.  The implementation remains HLAE / `-insecure` /
offline-demo only and does not rewrite the input demo.

## Confirmed reference behavior

- Empty T glove slots are represented by Pawn `m_EconGloves` state and are
  materialised as a complete target item, including definition, paint, seed,
  wear, account/initialisation fields, `m_nEconGlovesChanged`, and a synthetic
  item identity.
- Empty T knife 59 materialised as karambit 507 uses item high word
  `0xC52101FB`; the general observed namespace is
  `0xC5210000 | target_definition`.
- A real inventory knife keeps its original item identity while its target
  definition, finish, subclass/VData, model and animation resource context are
  changed coherently.
- The reference tool publishes an `AnimAssetData` target VNMSKEL entry before
  the rewritten entity is consumed. nSkinz independently supplies the same
  observable runtime requirement through its scoped serialization bridge.

## Ported runtime behavior

- `BuildSyntheticGloveEconItemIdentity` supplies a stable, non-reserved item
  identity to both Pawn and `CEconWearable` glove carriers.
- `ResolveKnifeEconItemHigh` preserves real knife identities and materialises
  default knives in the observed synthetic namespace.
- Holstered pre-arm and the active `BeforeWeapon` transaction publish the
  target definition, item identity and subclass/VData as one guarded change.
- The generic finish path reasserts the knife identity so the legacy firearm
  refresh path cannot replace it with the firearm sentinel.
- The native knife update probe records unfiltered first-update boundaries on
  a second demo load without writing to an unknown entity. This evidence is
  required before moving the transaction earlier.
- The standard firearm skin path is unchanged.

## Verified build at this checkpoint

- CS2 client `2000908`, patch `1.41.8.1`, source revision `10981323`.
- HLAE `2.192.2.0`.
- Full native regression suite passed.
- English DLL: 3,102,208 bytes,
  SHA-256 `D36360F5B8F85AD55CB5DF59EB5BC7200B3CFDE7C8C9E3E60775DE499801AE49`.
- Chinese DLL: 3,104,768 bytes,
  SHA-256 `2B1070C4C76AACEDAE9FC8A0A16688C8674A76B0ECAB53C0D8505929220959DD`.

## Remaining live evidence

The next cosmetics iteration must use a fresh log from this checkpoint build
after the same demo is loaded twice. The decisive fields are
`unfiltered_boundaries`, `unfiltered_weapon_updates`, the target knife
`item_id_high`, target subclass/VData verification, Pawn glove item identity,
and the native glove world/HUD refresh result. No speculative earlier write is
authorized without that evidence.
