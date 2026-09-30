# Workshop map-remap checkpoint (2026-09-21)

This checkpoint freezes the accepted map-remap implementation before work
returns to the independent cosmetics rewrite. The runtime remains
HLAE / `-insecure` / offline-demo only and never rewrites the input demo.

## Accepted live behavior

- The user confirmed that the Workshop radar is now normal.
- Map replacement is intentionally a second-load operation: the first demo
  load captures/stages state and the second `playdemo` parses the same demo
  with the selected Workshop map/addon identity.
- Workshop radar textures are decoded and compiled outside CS2, mounted as a
  native VTEX overlay, and selected on the second load.
- Radar controls are per configuration. Panorama caches a loaded virtual
  texture, so changing a slider regenerates the overlay but still requires a
  demo reload before the visible radar changes.
- Dark-road recovery is capped at 6 px. Values above 4 px require multi-axis
  enclosure so a long editor-grid line cannot be bridged back into the map.
- The dots/lines that remain continuous outside the circular HUD boundary are
  scene pixels visible through a fully transparent radar, not radar-mask data.

## Geometry and lighting safeguards

- Stale source-map generated model paths fail closed instead of guessing a
  target resource and crashing or hanging the map load.
- Standard target entity, probe-atlas and cubemap paths are routed only through
  the narrow map-runtime-data policy.
- Stock Nuke and Rainy Nuke were verified to share probe-volume geometry and
  atlas coordinates. Rainy Nuke may use the compatible stock Nuke probe atlas
  for dynamic demo models while preserving target static geometry, weather,
  post-processing, cubemaps and radar.
- Lighting audit now reports combined and ordinary probe volumes separately.
  Character darkness is not an acceptance blocker for this checkpoint.

## Verification frozen here

- English and Simplified Chinese Release builds succeeded.
- Full native regression suite passed.
- Both DLLs passed the six isolated loading cases plus the headless WARP/DXBC
  ImGui render/recreation test.
- English DLL SHA-256:
  `E26198F1FB90583C61374153613F12C6D578284C96B92D60659A353B0A85FFEC`.
- Chinese DLL SHA-256:
  `F06759F0A3653F8E9DEA12AF574086655779FE150C25795147DF7731EDB238E3`.
- Portable-load report:
  `build/diagnostics/source-probe-lighting-portable-load.json`.

## Resume boundary

Do not change the accepted radar path while continuing cosmetics work unless a
new live regression specifically implicates it. The next work item is the
two-load cosmetics transaction: first load observes/stages;
second load publishes complete knife model/animation and default-glove econ
state before the engine consumes those entities.
