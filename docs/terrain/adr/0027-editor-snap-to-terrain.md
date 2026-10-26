# 0027 �?Editor Snap to Terrain

Status: accepted for #335.

## Context

The editor must align a selected entity's authored Transform to the canonical
terrain surface without making `editor` depend on `terrain`, and without
introducing a second height formula. Terrain payloads arrive asynchronously, so a
snap may need several frames while the rest of the editor keeps running.

## Decision

- `core` owns a `GroundQuery` World resource and its `IGroundQuerySource` seam.
  A source answers `Sample` at the finest baked spacing and `Probe` from layout
  metadata only. `Sample` is `Ready` only when the returned spacing reached the
  tile's `sourceSpacing`; a coarser resident layer stays `Loading`.
- `terrain` provides the source-precision probe over `TerrainWorld`.
  `terrain_runtime` registers it through `Registration` and invalidates the
  query when a resolve or unload replaces the terrain.
- `editor` fits a double centroid-relative plane from a bottom-face footprint,
  selecting five corners-plus-centre samples or a nine-sample grid from the
  source spacing. A second query uses the first fitted rotation before any
  write. The final command sets position and rotation together through
  `AuthoredEdit`, so ordinary and Prefab undo/redo share one history entry.
- A pending snap retries outside panel drawing and cancels on timeout, terrain
  revision, selection/document/Play changes, or a Transform or parent change.
- A Prefab page has no scene terrain, so its action is disabled.

## Consequences

The editor never includes terrain headers. Terrain pays for one small bridge
adapter, and `GroundQuery` consumers must treat `Loading` as a normal state.
Undo/redo state is the existing authored data and override state; no separate
snap snapshot is stored.
