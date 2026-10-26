# scene

Saves and loads a scene, or one entity and its descendants, as data. It owns the
save-file identity, `StableId`, and the three-pass load. It owns no runtime state and no authoring UI.

`mir::scene` · depends `mir::core` (public); `mir::serial` (private) · API `modules/scene/include/scene/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

A scene is a directory, not a file.

```
scenes/<scene>/
  scene.json              manifest: ordered StableId array, mNextId, scene settings
  entities/
    <id>.json             one ordinary entity: {id, parent, siblingIndex, components[]}
  instances/
    <root>.json           one Prefab instance: prefab UUID, placement, bindings, patches
```

Splitting by entity keeps a diff proportional to what changed. Adding or removing
an entity is a file add or delete, and two people editing different entities
touch disjoint files, so their edits merge. A Prefab instance is one record
instead of one file per member, because its members are owned by a template; the
record stores each member's `StableId` binding and only the scene's overrides.

Components round-trip generically. Save walks every registered `ComponentOps`,
asks `Has`, and writes one value per `FieldDesc`. Load looks the type name up in
`ComponentRegistry` and writes each field back.

Giving a component a field table is therefore the entire serialization cost of
that component. It works the same way for a script-declared component, which has
no C++ type to hang a hand-written `ToJson`/`FromJson` pair on.

Load runs in three passes, because an `EntityRef` cannot resolve until every
entity in the file exists. First spawn every entity and write every
non-`EntityRef` field, building the `StableId` to `Entity` map. Then resolve every
`parent` through that map via `SetParent`. Then patch the recorded `EntityRef`
fields.

Sibling order comes back through the order of the first two passes. Creating an
entity appends it to the root list, and `SetParent` appends a child to its
parent. Both passes run in the order of `(siblingIndex, id)`, so one sort
restores the order of the roots and of every parent's children.
`SpawnEntities` installs the hierarchy before it creates anything, so a scene of
only roots still gets a root list.

An instance record is not an entity file. `LoadScene` reads every instance, hands
its record to a `ScenePrefabResolver`, and registers the materialized members
under the record's bindings before ordinary entity files are read. A binding
owns its `StableId`, so a stale member file cannot shadow the template. The
resolver also reports the template's default `EntityRef` targets; `SaveScene`
uses that to write a field patch only where the live value differs. A resolver
is optional: without one the record loads unchanged and nothing is materialized,
which is what an editor's preview World and a test that only needs the record
use.

`PlaySnapshot` is `SaveScene`/`LoadScene` reused for a second purpose: a
whole-World capture and restore around a Play session, rather than an
author's save file. `CaptureWorld` builds a throwaway `LoadedScene` covering
every entity `World::ForEachAliveEntity` finds — not just ones some other
`LoadedScene` tracks — and saves it to a scratch directory in the temp
directory, named by the process id, so two copies of a game entering Play at
once (a game build plays from its first frame) do not overwrite each other.
`DiscardSnapshot` removes it; `App` calls it at shutdown. `RestoreWorld`
destroys every entity currently alive, then `LoadScene`s that directory back,
returning the fresh `LoadedScene` identity (new `Entity` handles) for the
caller to adopt.

`EntitySnapshot` is the same per-entity data, cut down to one entity and its
descendants. Duplicate, undo of delete and prefabs build on it. `CaptureEntity`
walks the root and then `ForEachDescendant`, so every parent comes before its
children; `SnapshotMembers` returns that same list. Each member gets a
snapshot-local id, starting at 1 for the root.
The result is a JSON array in the entity-file format, kept in memory as compact
text. Text keeps nlohmann out of the public header, and a prefab can write it to
disk unchanged.

`RestoreEntity` runs the same three passes into fresh entities, so each
captured parent's children come back in their sibling order. Every
restored entity is registered in the caller's `LoadedScene` under a new
`StableId`, so it saves like any other. The root's own parent is not captured.
The caller passes one, and the root is appended as that parent's last child.

`RestoreEntityAt` is the same restore for undo, which brings back the entities
a delete removed rather than making copies. The caller passes one `StableId`
per member, in `SnapshotMembers` order, and each member is registered under its
own. It returns the members in that order. The root goes under `parent`, before
`before`, so it lands where it was. An anchor that is no longer a sibling there
still leaves the root under `parent`, last.

Both paths share `EntityToJson` and `SpawnEntities` in the private
`src/scene/EntityCodec.h`. `SaveScene` and `LoadScene` only add the files
around them.

## API

| Type | Header | Role |
|---|---|---|
| `StableId`, `kNullStableId` | `scene/SceneAsset.h` | `uint64_t` save-file identity, distinct from `Entity` |
| `LoadedScene` | `scene/SceneAsset.h` | `{mName, mEntities, mInstances, mUnresolvedEntityRefs, mNextId}` |
| `ScenePrefabInstance`, `SceneInstancePatch`, `SceneFieldPatch`, `SceneComponentRecord` | `scene/SceneAsset.h` | One instance record: Prefab UUID, placement, address-to-`StableId` bindings and scene overrides |
| `ScenePrefabResolver`, `InstancePlacementFromWorld`, `CaptureInstanceRecord`, `RestoreInstanceRecord` | `scene/SceneIO.h` | The seam a Prefab module implements, and the record capture/restore used by undo |
| `SetInstanceEntityRef` | `scene/SceneAsset.h` | Writes one member's `EntityRef` and records the override |
| `NewScene`, `AllocateStableId`, `CreateSceneEntity`, `DestroySceneEntity` | `scene/SceneAsset.h` | Authoring entry points |
| `SaveScene`, `LoadScene`, `UnloadScene` | `scene/SceneIO.h` | Directory in, directory out, scoped destroy |
| `ProbeScene`, `SceneProbe` | `scene/SceneIO.h` | What `LoadScene` would skip, found without a `World` |
| `PlaySnapshot`, `CaptureWorld`, `RestoreWorld`, `DiscardSnapshot` | `scene/PlaySnapshot.h` | Whole-World capture/restore around a Play session, built on `SaveScene`/`LoadScene`; `DiscardSnapshot` removes its directory |
| `SceneModule`, `SceneLoadResult`, `SceneLoadCause` | `scene/SceneModule.h` | The scene the World holds, its Play snapshot, and how it last got there |
| `EntitySnapshot`, `SnapshotMembers`, `CaptureEntity`, `RestoreEntity`, `RestoreEntityAt`, `DuplicateSceneEntity` | `scene/EntitySnapshot.h` | One entity and its descendants as in-memory data, restored into new entities registered in a `LoadedScene`, under new `StableId`s or, for undo, their old ones |

`SceneModule` holds the `LoadedScene`, the scene directory and the Play
snapshot. Its commands replace the scene without unloading it first, because
`App` runs `SceneUnload` between the two. Each command records a
`SceneLoadResult`: from disk, new, or restored from the snapshot, and whether it
is incomplete. `SceneLoad` stage functions read it through `LastLoad()`, and are
its only readers.

`CreateSceneEntity` is the one call an editor's "add entity" needs, so allocation
and registration cannot happen for one without the other.

`DestroySceneEntity` is its opposite. It removes the entity and every descendant
from `LoadedScene::mEntities`, then destroys the root, and the hierarchy's destroy
hook takes the descendants with it. It returns `false` for an entity that is not
alive.

`DuplicateSceneEntity` is `CaptureEntity` followed by `RestoreEntity` under the
source's own parent. The copy then moves to right after the source, as Unity's
Ctrl+D places it. It returns null for an entity that is not alive.

`StableIdOf` and `FindInstanceBinding` (`scene/SceneDuplicate.h`) answer which
scene id an entity has and which prefab instance binds it; the editor's delete
and reparent guards use them too.

`DuplicateSceneEntitySubtree` retains the linked Prefab record for an instance
root, while `CopyInstanceMemberAsOrdinary` produces a detached effective copy.
Both map every copied scene identity, including ordinary descendants. Saved
field and added-component references use that map; outside targets stay unchanged.
Unresolved reference markers are copied under the new owner IDs, so a temporarily
unavailable target is retained through Save instead of becoming an explicit null.

## Rules

**Never reference an entity by `Entity` in a file.** `Entity::mIndex` is a slot
that gets reused, and `mGeneration` guards against reading a stale one. Neither
survives a restart with the same meaning, so `parent` and every `EntityRef` are
written as `StableId`.

**A `StableId` is never reused, not even after its entity is deleted.** `mNextId`
is a high-water mark persisted in the manifest.

A stale reference in a file that has not been resaved therefore resolves to
"gone" rather than to whatever new entity took the id. `LoadScene` clamps
`mNextId` above every id actually present, in case the file was hand-edited.

Undo is the one exception. `RestoreEntityAt` gives a deleted entity its own id
back, because it is the same entity returning, so a save after a delete and its
undo writes the same files. Undoing a create does not lower `mNextId`, so that
id is not handed out again either. A `kNullStableId` in the list leaves its
member unregistered, for an entity that was never in the scene. An id that is
somehow already registered gets a new one, with a warning.

**`LoadedScene::mEntities` is a `std::map`, not an `unordered_map`.** The manifest
is written in key order, and an unordered map's iteration order is not stable
between runs, so writing from one would reshuffle the manifest on every save even
with no real change.

Sorted order also means a new entity, which always has a higher id, appends to
the array instead of reordering it.

**A binding, not an entity file, owns a Prefab member.** `SaveScene` writes one
`instances/<root>.json` per instance and skips every `StableId` that appears in
a binding. `LoadScene` reads the records first, so a stale member file cannot
materialize a second entity under the same id. A record whose Prefab is
unavailable keeps its bindings and loads nothing.

**A scene override stores presence, not difference.** A field patch equal to the
template value still saves, so a clear is distinct from never having overridden
the field. `SetInstanceEntityRef` writes a null override when the user clears a
reference; a missing target keeps its saved `StableId` in
`mUnresolvedEntityRefs` until it can resolve again.

**`siblingIndex` counts only the siblings being saved.** An entity outside the
save, such as an editor-only root, leaves no gap and does not shift the numbers.
Load sorts by `siblingIndex` and then by id. A missing index sorts after every
present one, so a scene saved before the field existed loads in id order, and a
duplicate index left by a merge falls back to id order too.

Load passes `keepWorld = false` to `SetParent`, because a saved `Transform` is
already parent-local.

**An unknown component name on load is skipped with a warning, not a load
failure.** That is forward compatibility for a file written by a newer build.

**A component with no `FieldDesc` table round-trips as present but empty.** It
keeps its existence and loses all its data, silently, and nothing enforces that a
field-bearing component carries a table.

**A `StableId` is scene-local.** An `EntityRef` pointing outside the scene being
saved is written as `kNullStableId` and loads back null, so cross-scene
references are not representable.

**An `EntityRef` leaving a snapshot keeps its live handle.** Duplicate and undo
restore within the same run, so the referenced entity can still exist. It is
written as `{"entity": PackEntity}` instead of an id. On restore it resolves
only if `IsAlive` still accepts that handle, and the generation check turns a
target destroyed since capture into null. `LoadScene` never resolves that form,
because a handle from a file names a different run's entity, so it loads null
with a warning.

**`RestoreEntity` does not resolve runtime handles.** Script instances, meshes
and materials are left out of the field tables. `renderer`'s
`MeshResolveSystem` picks up a restored `MeshRenderer` on the next frame, and
`script`'s `ScriptResolveSystem` a restored `ScriptRef`.

**`UnloadScene` destroys exactly what that load created**, each entity guarded by
`IsAlive` since destroying a parent already cascades. It never touches entities
that another loaded scene owns.

**`SaveScene` overwrites the directory** and returns `false` on any I/O failure.

**An entity file is named by its `StableId`, and `LoadScene` finds it by that
name.** The manifest lists ids, and each id maps to `entities/<id>.json`.

**`SaveScene` deletes the file of every entity no longer in the scene, and of
every instance record no longer present.** A deleted entity's file or a dropped
instance's record would otherwise stay on disk and in version control.
`LoadScene` would never read it, but a reader of the directory would take it for
a live object. The sweep runs after the manifest is written, so the manifest on
disk never lists a file the sweep removed. A crash before the sweep leaves stale
files, which load ignores. Only names of the form `<number>.json` are touched, so
anything else put in `entities/` or `instances/` survives. A file that cannot be
deleted logs a warning, and the save still succeeds.

**`LoadScene` is lenient, and `ProbeScene` says what it would skip.** A
`scene.json` that is unreadable or holds a wrongly typed value (a non-id in
`entities` or `instances`, an instance listed twice) loads an empty scene. An
unreadable entity or instance file is skipped. Prefab-instance members have no
entity file, so neither reads one for them. `ProbeScene` reads the same files
through the same readers without touching a `World` and reports all three cases. A caller probes first when it must decide before anything is
unloaded, which is what `App` does at startup and on the scene menu.

## State

*As of 2026-09-12.* Save, load and unload work end to end, including the generic
field-table round trip and the `EntityRef` patch pass. `PlaySnapshot` builds on
the same load/save, tested directly against a live `World` (mutate after
capture, restore, check it reverted) rather than through files.

*As of 2026-10-07.* `EntitySnapshot` captures and restores a subtree,
including sibling order, `EntityRef`s inside the snapshot, and live references
leaving it. The editor's Duplicate calls it through `DuplicateSceneEntity`;
undo and prefabs are separate work.

*As of 2026-10-08.* `DestroySceneEntity` backs the editor's Delete, and a save
after it removes the deleted entities' files.

*As of 2026-10-09.* Root and child order survive save, load and Play/Stop
through `siblingIndex`, and a duplicate lands right after its source.

*As of 2026-10-19.* `RestoreEntityAt` puts a deleted subtree back with its
`StableId`s and its sibling position, and the editor's undo uses it (see
[editor-history.md](editor-history.md)). A test saves a scene, deletes and
restores a middle child and a root, saves again and compares every file byte
for byte.

*As of 2026-10-23.* Instance records load through a `ScenePrefabResolver` and
round-trip their bindings, placement and field/component patches, including
nested occurrences, explicit nulls and cross-instance references. A missing
Prefab keeps the record unchanged.

Tests in `modules/scene/tests/SceneIO.tests.cpp`,
`modules/scene/tests/PlaySnapshot.tests.cpp` and
`modules/scene/tests/EntitySnapshot.tests.cpp`.

Resolving names back into live handles after a load is not done here — a
`ScriptRef.scriptName` into an `instanceRef`, a mesh name into a `MeshHandle`, or
a `TerrainInstance.recipePath` into a pinned terrain release with its meshes and
collision pieces. The modules that own those runtimes do it, because they need
the renderer and the script host that this module does not link. Scripts and
meshes resolve every frame, in `script`'s `ScriptResolveSystem` and `renderer`'s
`MeshResolveSystem`, so they cover every way an entity arrives. Terrain resolves
in `terrain_runtime`'s `SceneLoad` function (see
[terrain_runtime.md](terrain_runtime.md)).

## Backlog

1. **A defined failure mode for a missing entity file** whose `StableId` is still
   listed in the manifest. Skip with a warning, or abort the load — undecided.
2. **Cross-scene references.** They need either a global id namespace or an
   explicit external-reference `FieldKind`, decided before any save file starts
   relying on today's silent-null behaviour.
3. **Load is O(entities × registered components)** for the `Has` sweep. If that
   shows up in a profile on a large scene, have each entity file list its
   component names ahead of the blobs, so load can call `Find` per name instead
   of sweeping the whole registry per entity.

## Changed

*2026-10-24* — **`SceneModule`.** Was: `App` held the `LoadedScene` and the
Play snapshot and called `LoadScene`, `SaveScene`, `CaptureWorld` and
`RestoreWorld` itself. Now: the module owns them and records why the scene is
there for the `SceneLoad` stage. Because: the editor's history and unsaved mark
follow from that cause, and the editor must not need `App` to find it out.

*2026-10-24* — **`ProbeScene` checks a scene before it loads, and a wrongly typed
manifest value rejects the whole manifest.** Was: `LoadScene` skipped a bad
entity or instance id, an instance listed twice or a non-id `nextId` with a
warning, and nothing could tell a caller what a load would drop. Now: those
values make `scene.json` unreadable, and `ProbeScene` reports it along with
missing entity and instance files. Because: `App` refuses or asks about a broken
scene instead of loading part of it, so a silently skipped id would hide the
damage it exists to catch.

*2026-10-23* — **Scene Prefab instances.** Was: a scene saved one entity file
per entity, so a Prefab's members were flattened into the scene. Now:
`instances/<root>.json` stores the Prefab UUID, bindings and scene overrides,
and `LoadScene` materializes them through the resolver. Member entities are not
written as entity files, so a template and its instance never both own the same
component values.

*2026-10-19* — **Undo gives a deleted entity its `StableId` back.** Was:
`RestoreEntity` registered every restored entity under a new `StableId` and
appended the root as its parent's last child. A delete and its undo then showed
in the scene files as a delete plus an add, with the entity moved to the end.
Now: `RestoreEntityAt` takes each member's old id and an anchor sibling, so the
files come back unchanged. `RestoreEntity` and `DuplicateSceneEntity` still
hand out new ids. The rule that a `StableId` is never reused gains this one
exception, for the editor's undo (see [editor-history.md](editor-history.md)).

*2026-10-09* — **Entity files carry `siblingIndex`, and duplicates land after
their source.** Was: the manifest was written in id order and load replayed
`SetParent` in that order, so children came back in id order. Roots came back
in whatever order the slots fell, and Play then Stop scrambled both, because
`CaptureWorld` assigns ids in slot order. A duplicate went last among its
siblings, because `SetParent` could only append. Now: each file records its
position, load sorts by it, and `DuplicateSceneEntity` anchors the copy after
the source. The editor's drag reordering needed order to survive.

*2026-10-08* — **`SaveScene` deletes the files of deleted entities, and entity
files are named `<id>.json`.** Was: save wrote one file per entity still in the
scene and never removed any. A deleted entity's file stayed in `entities/` and in
version control. This doc also still described the old `<id>-<name>.json` naming.
Now: after the manifest is written, `SaveScene` deletes every `<number>.json`
whose id is not in the scene, and the Model and Rules describe the naming the
code uses. `DestroySceneEntity` and `DuplicateSceneEntity` were added for the
editor's Delete and Duplicate.

*2026-10-07* — **A non-integer `EntityRef` in a scene file loads null.** Was:
`LoadScene` read every `EntityRef` with `get<StableId>()`, so a `null` or a
string in its place threw `nlohmann::type_error` out of `LoadScene`. Now: the
load passes moved into `SpawnEntities`, shared with `EntitySnapshot`, and any
value that is not an id, including a snapshot's `{"entity": …}` form, loads
null with a warning. The load warnings now start with `SpawnEntities:` instead
of `LoadScene:`.

*2026-10-02* — **The Play snapshot directory is per process.** Was:
`%TEMP%/mjolnir_play_snapshot`, shared, so game builds started together, which
all enter Play at launch, failed to write it. Now:
`mjolnir_play_snapshot_<process id>`, removed by the new `DiscardSnapshot`,
which `App` calls at shutdown.

*2026-09-18* — **Scene files read and write through `mir::serial`, and a
malformed one no longer crashes.** Was: `SaveScene` and `LoadScene` used
`std::ofstream` and `std::ifstream` directly, and both read sites used nlohmann's
`operator>>` with no `try`/`catch`. A hand-edited `scene.json`, or any one
malformed entity file, threw out of `LoadScene` and reached `std::terminate`
through `main`. Now both go through `ReadJsonFile`/`WriteJsonFile`, a bad
manifest logs and loads an empty scene, and a bad entity file is skipped while
its valid siblings still load.

Writes are atomic per file but not across the set, which is in
[serial.md](serial.md)'s backlog.

`scene` also owns the manifest's filename now. `kSceneManifestFileName` and
`SceneManifestPath` are exported from `SceneIO.h`, because `app` was composing
`sceneDir / "scene.json"` itself to test whether a directory holds a scene, which
meant this module could not move its own manifest without breaking that check.

*2026-09-12* — **`PlaySnapshot` added, and the resolve-pass home question
resolved.** Was: `SaveScene`/`LoadScene` had one caller (`App::LoadScene`), and
this doc's backlog asked where `ResolveSceneScripts`/`ResolveSceneMeshes`
should live once a second caller existed. Now: `RestoreWorld` is that second
caller (Play→Edit), and both routes converge on `App`, which resolves names
back into handles after either one.

*2026-09-03* — **`EntityRef` fields stopped being written as bytes.** Was: the
generic path would have written every `FieldDesc` value directly, and an
`EntityRef` reads back a raw `Entity`, which is what a save file cannot store.
Now: translated through the same `Entity` to `StableId` map the entity writer
already builds, and patched in a third load pass.
