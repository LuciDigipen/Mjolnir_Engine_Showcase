# prefab

Prefab identity, source documents, compiled templates, scene instances and the
authoring state edits go to. It owns how a Prefab is stored, expanded and
instance-bound; it owns no editor panel and no World system beyond publishing
compiled edits at the Edit safe point.

`mir::prefab` · depends `mir::core`, `mir::assets`, `mir::scene`, `mir::serial` (public) · API `modules/prefab/include/prefab/` · maintainer Jackey Cen · reviewed 2026-10-23

## Model

PrefabModule owns setup and transition registrations. It scans sources in editor builds or loads UUID-addressed resources through AssetsModule in game builds. EditorModule owns the workspace previews and authoring adapter.

A Prefab has two identities. A `PrefabId` (UUID v4) is its persistent identity,
so renaming or moving the file keeps every reference. The file is a versioned
`.prefab` JSON document.

```
games/<game>/assets/prefabs/<name>.prefab
  formatVersion: 1
  prefabId:      the UUID
  nextNodeId:    decimal string high-water mark
  rootNode:      the root entity node
  nodes[]        entity nodes with component defaults, or nested `prefab` nodes
```

Every entity node has an id and an optional parent. A nested node names another
`PrefabId` and carries `patches` whose `target` is an address inside that
referenced document. Node ids, target segments and `nextNodeId` are strings, not
JSON numbers: a node id is 64 bits and JSON numbers lose precision above 2^53.

`PrefabFormat` parses, validates and writes that document. `PrefabRegistry`
holds documents from three places, in that lookup order: an unsaved in-memory
document, the source files an editor build scanned, or the cooked manifest a
game build loaded. Only the editor path has source paths; a cooked entry is
resolved by `PrefabAssetId(PrefabId)`, the string `prefab:<uuid>`.

`CompilePrefab` turns a document and the documents it nests into one flattened
`CompiledPrefab`:

- every entity becomes a `CompiledPrefabNode` whose `mAddress` is the nested
  occurrence ids followed by its own node id, so two House occurrences of Door
  have distinct addresses;
- each node carries component bytes already decoded from the document;
- every `EntityRef` becomes a `PrefabEntityRefRelocation` naming the target
  node, never a runtime `Entity`;
- `mDependencies` lists every Prefab the expansion used.

`CompiledPrefabCache` keeps the last valid template per id and bumps its revision
only on a successful compile. A failed compile leaves the previous template
alive.

A scene instance is a `ScenePrefabInstance` in `mir::scene`, keyed by the
instance root's `StableId`. It stores the Prefab UUID, parent, sibling index,
root Transform placement, one binding from every template address to a
`StableId`, and the scene's own field/component patches. `PrefabSceneResolver`
implements the scene's resolver seam: it compiles the template on demand,
instantiates it, and answers the template's default `EntityRef` targets so a
save can tell an override from an inherited value.

`PrefabCoordinator` owns the registry, compiled cache and resolver, and tracks
the bound scene and the preview Worlds owned by EditorModule's PrefabWorkspace. Its
`PrefabSystem` runs first in `PreUpdate` but commits only in Edit, so a Prefab
edit reaches instances at a frame boundary, never during Play. Frozen during
Play, it also queues source rescans until the next unfrozen commit.

The authoring layer works above the compiled data. `PrefabAuthoring` reads and
writes authored entries keyed by `{PrefabId or scene, node address, component,
field}`, which is what the editor's undo history records. `PrefabOverrides`
provides the scene-side operations for one instance: set or revert a field
override, add or remove a component, and apply selected overrides back into the
Prefab document.

```cpp
PrefabRegistry registry;
registry.Scan(prefabsDir);
CompiledPrefabCache cache;
const PrefabDocument *doc = registry.Find(prefab);
if (doc != nullptr)
    cache.Compile(*doc, RegistryLookup(registry));
const std::shared_ptr<const CompiledPrefab> compiled = cache.Find(prefab);
const PrefabInstantiation placed = InstantiatePrefab(world, *compiled);
```

`InstantiatePrefab` builds an independent entity graph and resolves every
relocation inside it. `PlacePrefabInstance` additionally registers each member
under a fresh `StableId` and records the scene instance.

## API

| Type | Header | Role |
|---|---|---|
| `PrefabId`, `GeneratePrefabId`, `ParsePrefabId`, `ToString` | `prefab/PrefabId.h` | 128-bit persistent identity and canonical text |
| `PrefabDocument`, `PrefabNode`, `PrefabPatch`, `NewPrefabDocument`, `AllocateNodeId` | `prefab/PrefabDocument.h` | In-memory source document and its high-water mark |
| `ParsePrefab`, `ValidatePrefab`, `PrefabToJson`, `ReadPrefabFile`, `WritePrefabFile` | `prefab/PrefabFormat.h` | Versioned `.prefab` text |
| `CompiledPrefab`, `CompiledPrefabCache`, `CompilePrefab`, `RegistryLookup` | `prefab/CompiledPrefab.h` | Flattened template, its relocations and revisioned cache |
| `PrefabRegistry`, `PrefabAssetId` | `prefab/PrefabRegistry.h` | Lookup by UUID from memory, source files or cooked assets |
| `PrefabAsset`, `BuildPrefabAsset`, `ReadPrefabAsset` | `prefab/PrefabAsset.h` | Cooked payload and its nested UUID list |
| `PrefabInstantiation`, `InstantiatePrefab` | `prefab/PrefabInstantiate.h` | A fresh entity graph from one template |
| `ScenePrefabResolver`, `PlacePrefabInstance` | `prefab/PrefabSceneInstance.h` | Materialize an instance and register its StableIds |
| `PrefabFieldState`, `SetInstanceFieldOverride`, `RevertInstancePatch`, `ApplyPrefabInstancePatches` | `prefab/PrefabOverrides.h` | One scene instance's overrides |
| `PrefabAuthoring`, `PrefabAuthoredKey`, `PrefabAuthoredEntry` | `prefab/PrefabAuthoring.h` | Document-qualified authored state for undo |
| `PrefabModule` | `prefab/PrefabModule.h` | Source/cooked setup and scene/Play transition registrations |
| `PrefabCoordinator`, `PrefabSystem` | `prefab/PrefabCoordinator.h` | Registry, cache, resolver, previews and commit |
| `PrefabCaptureResult`, `CapturePrefab`, `UniquePrefabPath` | `prefab/PrefabCapture.h` | A scene subtree as a new document |
| `PlanSavePrefab`, `PlanSaveScene`, `ExecuteSavePlan` | `prefab/PrefabSave.h` | Dependency-ordered writes |

## Rules

**Identity is the UUID, not the path.** The cooker stores a Prefab under
`PrefabAssetId(id)`, so moving or renaming the file changes no reference. A
duplicate UUID in two files resolves from neither, and the cooker refuses it.

**Node ids are never reused.** `nextNodeId` is a persisted high-water mark, so a
deleted node's id cannot come back and silently match a stale reference.

**A Prefab `EntityRef` is a node address; a scene `EntityRef` is a `StableId`.**
Neither is ever a runtime `Entity`. In a Prefab, a target with several segments
passes through nested occurrences; in the scene, the flat compiled address is
what an instance binding stores.

**A nested patch is relative to the inner document.** `[1]` in a House patch on
a Door occurrence means the Door root, not the House root. Compilation prefixes
the occurrence id once, so the same address is `[occurrence, 1]` in the
compiled template and in the scene instance.

**An override is a stored presence, not a value comparison.** A scene patch that
equals the template value still saves, so authoring intent survives; reverting
it removes the patch.

**An instance binding owns its members' ids.** `SaveScene` writes one
`instances/<root>.json` and no entity files for the bound members, so a stale
member file cannot shadow the template. A missing or invalid Prefab keeps the
record and its bindings unchanged.

**A Prefab that cannot compile publishes nothing.** Compilation errors leave the
last valid template and its revision in place; unknown component types are
warnings and are kept verbatim in the document.

**Play freezes publication.** `PrefabSystem` commits only in Edit. During Play
the compiled state and the scene instances are pinned, and a source rescan waits
for the next unfrozen commit.

**A preview is its own World and scene.** Its entities never enter the main
scene or Play snapshot. Successful document edits propagate to bound instances
through the shared coordinator at the Edit safe point.

**A game build reads cooked Prefabs only.** Without the editor, the registry is
filled by `LoadCooked` from the cooked manifest; the build removes the copied
`prefabs` source folder next to the executable, so no source path is reachable.

## State

*As of 2026-10-23.* Source scan, cooked load, compile, nesting, previews,
scene instances, overrides, capture, save plans and authored state work. Tests
in `modules/prefab/tests/` cover the format, identity, registry, cooking,
compilation and nesting, instantiation, scene round trips, overrides, capture,
save order, coordinator publication and authoring. `modules/scene/tests/` covers
instance records and references, and `modules/editor/tests/` covers the open
documents and authored adapter.

`games/PrefabCheck` is the packaged acceptance: two crate instances, a House
with two nested Door occurrences and overrides, a null reference, a
cross-instance reference and a detached copy. Its Lua check runs in
`smoke.PrefabCheck` from a no-editor build whose source Prefabs have been
removed. Save and load round trips remain covered by the module tests; a game
build has no save path.

## Backlog

1. **Moving a node between Prefabs** would need an explicit remap step, since a
   node address belongs to one document.
2. **A visual diff between an instance and its template** is still the
   editor's job; the module only reports `PrefabFieldState` and
   `PrefabComponentState`.

## Changed

*2026-10-23* — **Cooked Prefab loading.** Was: the registry only scanned source
files, so a build without the editor had no Prefabs. Now: `PrefabAsset` stores
a document under its UUID AssetId, `LoadCooked` fills the disk table from the
manifest, and the game build removes the copied source folder.

*2026-10-23* — **Nested Prefabs and scene instances.** Was: an instance was one
document's nodes. Now: `CompiledPrefab` expands nested occurrences, every node
has a flat address, and a scene instance binds each address to a `StableId`.

*2026-10-21* — **Authored state and previews.** Was: edits were scene patches
only. Now: `PrefabAuthoring` records authored entries across Prefab documents
and the scene, and each open document has an isolated preview World.
