# editor-history

The editor's undo and redo: one history of steps, the commands that make them,
and the id table that lets a step find its entity again after a delete and its
undo. It is part of `editor` (see [editor.md](editor.md)). It never links
`mir::scene`, and it reaches the scene only through `ISceneOps`, which `App`
implements.

`mir::editor` · depends `mir::core` · API `modules/editor/include/editor/history/` · maintainer Dong Ta · reviewed 2026-10-20

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
stack, so the budget, the Edit menu labels and a future save point all read one
container.

A step holds its command, the selection before and after it, its size and a
serial number.

Deleting an entity frees its handle (`Entity {index, generation}`), and undo
brings it back under a new one. So a command never holds an `Entity` past its
first `Do`. It holds a `HistoryId`, history's own number for an entity, and
`EntityIds` maps each id to the entity's current handle. Ids are never reused.
An undone delete calls `Rebind`, and every later step that looks the id up finds
the restored entity.

Entity operations need the `LoadedScene` and the snapshot functions, which live
in `mir::scene`. The editor does not link it, so it asks through `ISceneOps`:
create, duplicate, capture, restore and destroy. `App` implements it as the
private `AppSceneOps` and hands it to `Editor::Initialize`.

## API

| Type | Header | Role |
|---|---|---|
| `History`, `kHistoryBudget` | `editor/history/History.h` | Steps, cursor, queue and budget: `Submit`, `EndStep`, `RequestUndo`, `RequestRedo`, `Flush`, `Clear`, and the names the Edit menu shows |
| `ICommand`, `CommandContext` | `editor/history/Command.h` | One undoable change, and the `{World, EntityIds, ISceneOps}` it receives when it runs; `TryMerge` folds the next command of a drag into it |
| `FieldValue`, `MakeSetFieldsCommand`, `MakeAddComponentCommand`, `MakeRemoveComponentCommand` | `editor/history/ComponentCommands.h` | The Inspector's and the gizmo's edits as commands; a `FieldValue` is one field's new bytes, by index into `ComponentOps::mFields` |
| `EntityIds`, `HistoryId`, `kNullHistoryId` | `editor/history/EntityIds.h` | History id to current `Entity`; `IdOf`, `EntityOf`, `Rebind`, `Unbind`, `Retain` |
| `ISceneOps`, `CapturedSubtree` | `editor/history/SceneOps.h` | The scene calls history needs, and a captured subtree: its snapshot text, members and their `StableId`s |
| `MakeCreateCommand`, `MakeDeleteCommand`, `MakeDuplicateCommand`, `MakeReParentCommand`, `MakeEntityCommand` | `editor/history/EntityCommands.h` | The Hierarchy's four entity operations as commands; `MakeEntityCommand` maps an `EntityAction` and returns null for `None` |

The command classes are private to `EntityCommands.cpp` and
`ComponentCommands.cpp`. Callers only ever hold a `unique_ptr<ICommand>`, and
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

Set fields is every field edit: one field from an Inspector widget, the one
`Transform` channel a gizmo operation moves, the picked asset plus the fields its
`mResetOnChange` clears, or every field Paste Values writes. Paste onto an entity
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

**One press-to-release drag is one step, and two drags are two.** Only the newest
step is open, and only while the cursor is at the end. Set fields merges when the
next command edits the same fields of the same component on the same entity.
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

**Ctrl+Z and Ctrl+Y do nothing while a text field is active.** The field has
its own undo. Holding either key repeats it. Ctrl+Y is redo because Windows
Explorer is the editor's UI reference.

**Commands change component data only.** Physics bodies, resolved meshes and
GPU resources are rebuilt by their systems, as after a load. Scripts are the
exception, because `ScriptSystem` skips a `ScriptRef` without an instance. So
`AppSceneOps` runs `ResolveSceneScripts` after a restore and after a duplicate.
Undoing Remove Component on a `ScriptRef` is not covered: it comes back with
no instance, as Add Component's does.

## State

*As of 2026-10-19.* Create, Delete, Duplicate and ReParent from the Hierarchy
go through history (#203), with Ctrl+Z, Ctrl+Y and the Edit menu. History
clears on New, Load, Play and Stop.

*As of 2026-10-20.* Inspector field edits, Paste Values, Add Component, Remove
Component and the gizmo's move, rotate and scale go through history too (#204).
No unsaved marker reads the step serial yet (#206).

Tests are in `modules/editor/tests/`. `History.tests.cpp` covers the cursor,
dropped redo steps, refused commands, step names, selection, the budget, id
clean-up and merging, through fake commands. `EntityCommands.tests.cpp` and
`ComponentCommands.tests.cpp` run every command through Do, Undo and Redo
against a `TestSceneOps` built on the real `mir::scene` functions, set up in
`EditingFixture.h`. They compare the saved scene files at each stage, rather
than component bytes, because a restored entity has new handles. The parts that
need ImGui or `App` have no unit test: where the Inspector and the gizmo call
`EndStep`, the shortcuts, the Edit menu and `AppSceneOps`. The `smoke.<game>`
runs start the editor with them in place.

## Backlog

1. **The unsaved marker (#206)** from the step serial and a save point.
2. **Delete and Duplicate act on one entity**, as the Hierarchy does today
   (editor.md backlog). Doing the whole selection would want one command that
   holds several records, so it stays one step.
3. **A preset that spawns entities outside the new entity's subtree** would
   leave them behind on undo, because only the subtree is captured. No preset
   does today.
4. **Pick the budget from measured snapshot sizes.** 64 MiB is a guess.
5. **A removed `ScriptRef` comes back without its script instance.** Resolving
   scripts after Add and Remove Component would need a scene op for it.

## Changed

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
