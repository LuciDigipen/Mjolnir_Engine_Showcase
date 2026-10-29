# editor-history

The editor's undo and redo: one history of steps, the commands that make them,
and the id table that lets a step find its entity again after a delete and its
undo. It is part of `editor` (see [editor.md](editor.md)). It never links
`mir::scene` or `mir::prefab`. It reaches the scene's entities only through
`ISceneOps`, and authored state in any document, such as Prefab overrides,
only through `IAuthoringBackend`. `App` supplies both.

`mir::editor` · depends `mir::core` · API `modules/editor/include/editor/history/` · maintainer Dong Ta · reviewed 2026-10-21

## Model

A panel never changes the `World` for an undoable edit. It builds a command
that knows how to apply one change and how to reverse it, and hands it to
`History`. `Editor` owns the one `History`. It runs the queued commands once per
frame, at the end of `DrawLayout`, after every panel has drawn and before any
system runs.

```cpp
if (ImGui::Begin("Hierarchy"))
    mHistory.Submit(MakeEntityCommand(mHierarchyPanel.Draw(world)));
...
//the safe point: every panel has drawn and no system has run yet
if (mSceneOps != nullptr)
    mHistory.Flush(world, *mSceneOps);
```

The Hierarchy, the Inspector and the viewport gizmo all submit this way, so no
editor panel writes the `World` for a scene edit itself.

`Flush` runs this frame's commands in order, then the undos and redos asked for
this frame. A command whose `Do` returns true becomes a step. Ctrl+Z undoes one
step and Ctrl+Y redoes it.

A drag submits a command every frame, and one press-to-release drag has to be one
step. So the newest step stays open, and while it is open a new command whose
`TryMerge` it accepts folds into it: the step keeps its first before and takes
the newest after. The panel calls `EndStep` when the widget or the gizmo is let
go, and the next submit starts a new step.

History is one list of steps plus a cursor, the number of steps applied. Undo
runs `Undo` on the step before the cursor and moves the cursor back. Redo runs
`Do` on the step at the cursor and moves it forward. A new step first drops
every step from the cursor on. One list rather than an undo stack and a redo
stack, so the budget, the Edit menu labels and the save point all read one
container.

A step holds its command, the selection before and after it, its size, a
serial number and the documents it changes. Serials only go up, and the unsaved
marker is built on them: a save records, for its document, the serial of the
newest applied step that changed that document, and the document is unsaved
whenever that serial is another one. Scene commands change the scene document,
`kSceneDocument`.

Prefabs add more documents than the scene (see the Prefab design, #230). An
authored command changes authored entries, not `World` data: an override's
presence and value, a component's added or removed-inherited state, or an
object's own record. Each entry is keyed by document, logical object (an address
the backend defines, such as an instance root `StableId` followed by a Prefab
node address), component and field. The command reads its befores and writes
its changes through `IAuthoringBackend`, which validates the whole batch across
its documents and writes all of it or none. One Apply that writes a Prefab and
clears the scene's override is one step.

Deleting an entity frees its handle (`Entity {index, generation}`), and undo
brings it back under a new one. So a command never holds an `Entity` past its
first `Do`. It holds a `HistoryId`, history's own number for an entity, and
`EntityIds` maps each id to the entity's current handle. Ids are never reused.
An undone delete calls `Rebind`, and every later step that looks the id up finds
the restored entity.

Entity operations need the `LoadedScene` and the snapshot functions, which live
in `mir::scene`. The editor does not link it, so it asks through `ISceneOps`:
create, duplicate, capture, restore and destroy. `EditorModule` implements it
as `WorldSceneOps` and hands it to `Editor::Initialize`.

## API

| Type | Header | Role |
|---|---|---|
| `History`, `kHistoryBudget` | `editor/history/History.h` | Steps, cursor, queue and budget: `Submit`, `EndStep`, `RequestUndo`, `RequestRedo`, `Flush`, `Clear`, the names the Edit menu shows, each document's unsaved marker (`IsUnsaved`, `SetUnsaved`), and `SetAuthoringBackend` |
| `ICommand`, `CommandContext` | `editor/history/Command.h` | One undoable change, and the `{World, EntityIds, ISceneOps, IAuthoringBackend*}` it receives when it runs; `TryMerge` folds the next command of a drag into it; `CollectDocuments` and `CollectMaintained` name the documents it changes |
| `DocumentId`, `kSceneDocument` | `editor/history/DocumentId.h` | 128 bits naming an authoring document, so a Prefab's UUID fits; the null id is the scene |
| `AuthoredObject`, `AuthoredKey`, `AuthoredEntry`, `AuthoringWrite`, `IAuthoringBackend`, `MakeAuthoredCommand` | `editor/history/Authoring.h` | The authored-state seam: what an entry is keyed by, the backend App supplies, and the command that changes entries across documents |
| `FieldValue`, `MakeSetFieldsCommand`, `MakeAddComponentCommand`, `MakeRemoveComponentCommand` | `editor/history/ComponentCommands.h` | The Inspector's and the gizmo's edits as commands; a `FieldValue` is one field's new bytes, by index into `ComponentOps::mFields` |
| `EntityIds`, `HistoryId`, `kNullHistoryId` | `editor/history/EntityIds.h` | History id to current `Entity`; `IdOf`, `EntityOf`, `Rebind`, `Unbind`, `Retain` |
| `ISceneOps`, `CapturedSubtree` | `editor/history/SceneOps.h` | The scene calls history needs, and a captured subtree: its snapshot text, members and their `StableId`s |
| `MakeCreateCommand`, `MakeDeleteCommand`, `MakeDuplicateCommand`, `MakeReParentCommand`, `MakeEntityCommand` | `editor/history/EntityCommands.h` | The Hierarchy's four entity operations as commands; `MakeEntityCommand` maps an `EntityAction` and returns null for `None` |
| `MakeDocumentEntityCommand` | `editor/history/DocumentNodeCommands.h` | Create, delete and reparent nodes in the active Prefab document as one authored command each |
| `MakeSetFieldsEditCommand`, `MakeAddComponentEditCommand`, `MakeRemoveComponentEditCommand`, `MakeCreatePrefabCommand`, `MakeRevertAuthoredCommand`, `MakeApplyAuthoredCommand` | `editor/history/AuthoredEdit.h` | Routes an Inspector, gizmo or Hierarchy edit to the authored backend for an instance member, and makes Create Prefab one step |

The command classes are private to `EntityCommands.cpp`, `ComponentCommands.cpp`
and `AuthoredCommand.cpp`. Callers only ever hold a `unique_ptr<ICommand>`, and
`SubtreeRecord`, in `src/editor/history/`, stays out of the public headers.

| Command | First `Do` | `Undo` | Redo |
|---|---|---|---|
| Create | `CreateEntity`, the preset, `AddMissingBaseline`; selects it | removes the subtree | restores it |
| Delete | removes the subtree; selects `NeighborInHierarchy` if the target was selected | restores it | removes it again |
| Duplicate | `Duplicate`; selects the copy | removes the copy | restores it |
| ReParent | moves each entity as the Hierarchy asked; selects them | moves them back in reverse order | moves them again |
| Set fields | reads each field's before, writes the new values | writes the befores | writes the new values |
| Add component | adds it with its defaults, then any pasted values | removes it | adds it again |
| Remove component | reads every writable field, then removes it | adds it back with those values | reads and removes it again |
| Authored change | reads the befores of the keys it names, then writes its changes and any maintenance as one batch | writes the befores, not the maintenance | writes the changes and maintenance again |

Set fields is every field edit: one field from an Inspector widget, the one
`Transform` channel a gizmo operation moves, the picked asset plus the fields its
`mResetOnChange` clears, a picked sound event plus its `mBankField`, or every
field Paste Values writes. Paste onto an entity
that lacks the component is Add component with the copied values.

`SubtreeRecord` is the remove and restore that Create, Delete and Duplicate
share. Remove captures the subtree through `ISceneOps::Capture`, gives each
member a history id, notes the parent and the next sibling, records every
reference crossing the subtree's edge, then destroys it. Restore asks
`ISceneOps::Restore` to rebuild it under the same parent, before the same
sibling, with the same `StableId`s. It then rebinds each member's id and
rewrites the recorded references.

## Rules

**A command holds history ids, never handles, after its first `Do`.** The
first `Do` turns the handles it was built with into ids. A handle kept longer
goes stale the first time its entity is deleted and restored, and the generation
check then turns it into nothing.

**`Do` returns false when nothing changed, and no step is recorded.** That
covers a preset whose `mCanCreate` refuses, a drop `CanSetParentAll` rejects, a
drop that leaves every entity with the same parent and next sibling, a field set
to the value it already has, adding a component the entity has, removing one it
lacks, and a target that is already dead. The redo steps survive, because
nothing new happened.

**`Undo` returning false leaves the cursor where it was.** So does a failed
redo. An authored command fails when the backend refuses its batch, and then
nothing in any document changed. Scene commands always report success: a target
that is gone is skipped with a warning, as before #243.

**One press-to-release drag is one step, and two drags are two.** Only the newest
step is open, and only while the cursor is at the end. Set fields merges when the
next command edits the same fields of the same component on the same entity. An
authored change merges when every entry has the same document, object,
component and field, and neither side carries maintenance.
`EndStep` closes the step after the frame's queue has run, so a release in the
same frame as the last change still keeps that change. Undo and redo close it
too. The Inspector calls `EndStep` once no ImGui item is active after it
submitted, which covers drags, typing, checkboxes, combos and menu items alike.
The gizmo calls it when `ImGuizmo::IsUsing()` turns false. Without the call, a
second drag of the same field would fold into the first, which is what the
two-steps test catches.

**Field values go through `FieldDesc::Write`, never as raw component bytes.**
`Transform`'s setters bump its version, so `WorldTransform` rebuilds; copying
the bytes would copy the version too. An `EntityRef` value is kept as a history
id, not a handle, so a field pointing at an entity that is deleted and restored
in between still finds it.

**An authored command captures only the keys it names, and presence counts.**
It never reads a whole component, so untouched fields and runtime state such as
resolved handles never become overrides. An entry's state is presence plus
value: adding an override equal to the inherited value is a change, undo makes
that field inherit again rather than keep an equal override, and writing the
override an entry already has is no step. A missing object refuses the whole
batch before anything is written. Without a backend, every authored command is
refused.

**Undoing a delete gives the entities back their `StableId`s.** Saving after a
delete and its undo leaves every scene file unchanged. Undoing a create or a
duplicate does not lower the manifest's `nextId`, so an id handed out once is
never handed out again, which is [scene.md](scene.md)'s rule.

**References across the removed subtree's edge are recorded both ways.** Remove
finds every `EntityRef` field pointing from outside into the subtree, and from
inside it out. Restore writes each one through the id table. The way in is what
makes another entity point at the restored one again. The way out is needed
because a snapshot keeps an outgoing reference as a live handle. After its
target is itself deleted and restored, that handle is stale. Delete B, delete A
which points at B, then undo twice, and A's reference would come back null.

**ReParent undoes in reverse order, and writes the old `Transform` back.** Each
move records the entity's old parent and old next sibling. Undoing move *i*
happens when the scene is exactly as it was just after move *i*, so that
sibling is still in place to insert before. The first move runs `SetParent`
with the world pose kept, which changes the local `Transform`. Undo writes the
saved local values back through the setters instead of recomputing them. A
recomputed float can differ in the last bit and change the saved file, and
copying the struct would copy its version stamp, so `WorldTransform` might not
rebuild.

**Selection is not a step.** Each step keeps the selection before and after it
as history ids, and undo or redo puts the matching one back. A dead entity drops
out of it. Clicking around fills no history.

**The budget bounds memory, not step count.** `kHistoryBudget` is 64 MiB. Past
it, the oldest applied steps go first, and the newest step always stays, even
alone over the budget. A step is measured again after every `Do` and `Undo`,
because a command's size changes when it runs: a delete captures its snapshot
then, and an undone create captures one. Ids no remaining step holds are
dropped whenever a step goes.

**History is cleared on New Scene, Load Scene, Play and Stop.** Each one
replaces the entities every step points at. Stop rebuilds the `World` from the
Play snapshot, so every handle history holds is dead afterwards. Edits made
during Play are undoable until Stop. Opening another project restarts the
process, which drops history with it.

**The unsaved marker follows the save point, not the steps.** `SetUnsaved(false)`,
which App calls after a save, records the serial of the step before the cursor
and closes the open step, so a drag after the save makes a new step with a new
serial. Undoing back to that step clears the marker, and undoing or redoing
past it shows it again. A saved step can drop out of history two ways: trimmed
for the budget, or dropped as a redo step by a new edit. Either way the marker
stays on until the next save, even if undo reaches a state that matches the
file, because history can no longer prove it does. `Clear` keeps the marker as
it was: an empty history gets a fresh serial, which is the save point when the
scene was saved and nothing when it was not. New and Load then call
`SetUnsaved(false)`, since the scene matches its file. Play remembers the
marker and Stop puts it back, because Stop puts the `World` back as it was at
Play; edits made during Play show the marker until then. The marker counts only
what goes through history.

**Each document has its own save point, and maintenance keeps it unsaved.** A
step moves the save state of the documents it changes and no others, so an
edit to a Prefab leaves the scene's marker alone. Maintenance is a persistent
change Undo deliberately leaves, such as clearing a scene's reference into a
deleted Prefab node (#230's source-deletion rule). An authored command declares
it alongside its changes, or the backend reports it from a write; either way
its documents stay unsaved until they are saved, even after Undo has returned
every other document to its save point. Undoing such a deletion writes back
only the command's own entries, so the cleared reference stays null. #203's
scene Delete is not affected: its undo still restores references into the
deleted entities.

**Ctrl+Z and Ctrl+Y do nothing while a text field is active.** The field has
its own undo. Holding either key repeats it. Ctrl+Y is redo because Windows
Explorer is the editor's UI reference.

**Commands change component data only.** Physics bodies, resolved meshes and
GPU resources are rebuilt by their systems, as after a load. Scripts are the
exception only in appearance: a restored or duplicated `ScriptRef` comes back
with no instance, and `ScriptResolveSystem` gives it one on the next frame, as it
does for Add Component and an undone Remove Component.

## State

*As of 2026-10-19.* Create, Delete, Duplicate and ReParent from the Hierarchy
go through history (#203), with Ctrl+Z, Ctrl+Y and the Edit menu. History
clears on New, Load, Play and Stop.

*As of 2026-10-20.* Inspector field edits, Paste Values, Add Component, Remove
Component and the gizmo's move, rotate and scale go through history too (#204).
The title bar shows `*` after the game's title while the scene is unsaved
(#206).

*As of 2026-10-21.* The authored-state seam exists (#243): document-qualified
authored commands, atomic batches across documents, refusable undo and redo,
and a save point plus maintenance flag per document.

*As of 2026-10-23.* App supplies the backend (#249):
`PrefabAuthoringAdapter` implements both `IAuthoringBackend` and
`IAuthoredEntities`, so Inspector overrides, Apply to Prefab, Revert and
component changes on instance members are authored steps. `DocumentNodeCommands`
makes node create, delete and reparent in the active Prefab document undoable,
and `MakeCreatePrefabCommand` makes Create Prefab one step over its new
document. Undoing a source-node deletion restores the nodes but leaves the
references it cleared null, as maintenance, until a later save.

Tests are in `modules/editor/tests/`. `Authoring.tests.cpp` runs authored
commands against a fake backend: inherited and equal-value overrides, component
states, batches across documents, failed targets, refused undo and redo,
merges, and per-document dirty state with maintenance. `DocumentNodeCommands.tests.cpp`
covers node create, delete and reparent in a document. `App`'s real backend and
open-document previews are tested in `modules/editor/tests/PrefabAuthoringAdapter.tests.cpp`
and `modules/editor/tests/PrefabWorkspace.tests.cpp`. `History.tests.cpp` covers the cursor,
dropped redo steps, refused commands, step names, selection, the budget, id
clean-up and merging, through fake commands. `EntityCommands.tests.cpp` and
`ComponentCommands.tests.cpp` run every command through Do, Undo and Redo
against a `TestSceneOps` built on the real `mir::scene` functions, set up in
`EditingFixture.h`. They compare the saved scene files at each stage, rather
than component bytes, because a restored entity has new handles. The parts that
need ImGui or `App` have no unit test: where the Inspector and the gizmo call
`EndStep`, the shortcuts, the Edit menu and `WorldSceneOps`. The `smoke.<game>`
runs start the editor with them in place.

## Backlog

1. **Warn about unsaved changes before they are lost.** New Scene, Load Scene
   and Open Project could ask first while the marker is on; today only Open
   Project warns, and it always does.
2. **Delete and Duplicate act on one entity**, as the Hierarchy does today
   (editor.md backlog). Doing the whole selection would want one command that
   holds several records, so it stays one step.
3. **A preset that spawns entities outside the new entity's subtree** would
   leave them behind on undo, because only the subtree is captured. No preset
   does today.
4. **Pick the budget from measured snapshot sizes.** 64 MiB is a guess.
5. **A removed `ScriptRef` comes back without its script instance.** Resolving
   scripts after Add and Remove Component would need a scene op for it.
6. **Undoing a scene Delete of a Prefab instance loses its instance record.**
   `DestroySceneEntity` erases the `ScenePrefabInstance` with its bindings and
   patches, and `SubtreeRecord` captures only entities. After Delete and Undo,
   a save writes plain entities. `ISceneOps::Capture` and `Restore` should carry
   the subtree's instance records too.

## Changed

*2026-10-23* — **The Prefab backend is connected (#249).** Was: authored
commands had a seam but no implementation, so they were refused. Now: App's
`PrefabAuthoringAdapter` supplies both interfaces, Inspector and gizmo edits on
instance members are authored steps, and Prefab node create/delete/reparent and
Create Prefab join the same history with their own document save points.

*2026-10-21* — **Authored-state history across documents (#243).** Was: every
step changed the scene, history had one save point, and `ICommand::Undo`
returned nothing, so an undo always moved the cursor. Now: `Authoring.h` adds
the backend seam and `MakeAuthoredCommand`; steps record the documents they
change; each document has its own save point and maintenance flag; and `Undo`
returns false to leave the cursor in place. The scene's marker and every scene
command behave as before.

*2026-10-20* — **Unsaved-changes marker (#206).** Was: each step had a serial
that nothing read, and nothing showed whether the scene differed from its file.
Now: `History` keeps a save point by serial, App sets it on Save, New and Load
and restores it on Stop, and the title bar shows `*` while the scene is unsaved.

*2026-10-20* — **Inspector and gizmo edits are undoable (#204).** Was: the
Inspector called `FieldDesc::Write`, `AddDefault` and `Remove` on the `World`
itself, and the gizmo called `Transform`'s setters, so none of it could be
undone. Now: they submit Set fields, Add component and Remove component
commands, and `History` merges the frames of one drag into one step through
`TryMerge` and `EndStep`.

*2026-10-19* — **Added.** Was: no undo anywhere in the engine. The Hierarchy
returned an `EntityAction` through `EditorActions`, and `App::ApplyEntityAction`
changed the scene directly. Now: `Editor` turns it into a command, `History`
runs it, and `App` only supplies `AppSceneOps`. The design that came before this
page is folded into Model and Rules.
