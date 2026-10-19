# editor-history

The editor's undo and redo: one history of steps, the commands that make them,
and the id table that lets a step find its entity again after a delete and its
undo. It is part of `editor` (see [editor.md](editor.md)). It never links
`mir::scene`, and it reaches the scene only through `ISceneOps`, which `App`
implements.

`mir::editor` · depends `mir::core` · API `modules/editor/include/editor/history/` · maintainer Dong Ta · reviewed 2026-10-19

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

`Flush` runs this frame's commands in order, then the undos and redos asked for
this frame. A command whose `Do` returns true becomes a step. Ctrl+Z undoes one
step and Ctrl+Y redoes it.

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
| `History`, `kHistoryBudget` | `editor/history/History.h` | Steps, cursor, queue and budget: `Submit`, `RequestUndo`, `RequestRedo`, `Flush`, `Clear`, and the names the Edit menu shows |
| `ICommand`, `CommandContext` | `editor/history/Command.h` | One undoable change, and the `{World, EntityIds, ISceneOps}` it receives when it runs |
| `EntityIds`, `HistoryId`, `kNullHistoryId` | `editor/history/EntityIds.h` | History id to current `Entity`; `IdOf`, `EntityOf`, `Rebind`, `Unbind`, `Retain` |
| `ISceneOps`, `CapturedSubtree` | `editor/history/SceneOps.h` | The scene calls history needs, and a captured subtree: its snapshot text, members and their `StableId`s |
| `MakeCreateCommand`, `MakeDeleteCommand`, `MakeDuplicateCommand`, `MakeReParentCommand`, `MakeEntityCommand` | `editor/history/EntityCommands.h` | The Hierarchy's four entity operations as commands; `MakeEntityCommand` maps an `EntityAction` and returns null for `None` |

The command classes are private to `EntityCommands.cpp`. Callers only ever hold
a `unique_ptr<ICommand>`, and `SubtreeRecord`, in `src/editor/history/`, stays
out of the public headers.

| Command | First `Do` | `Undo` | Redo |
|---|---|---|---|
| Create | `CreateEntity`, the preset, `AddMissingBaseline`; selects it | removes the subtree | restores it |
| Delete | removes the subtree; selects `NeighborInHierarchy` if the target was selected | restores it | removes it again |
| Duplicate | `Duplicate`; selects the copy | removes the copy | restores it |
| ReParent | moves each entity as the Hierarchy asked; selects them | moves them back in reverse order | moves them again |

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
covers a preset whose `mCanCreate` refuses, a drop `CanSetParentAll` rejects, and
a target that is already dead. The redo steps survive, because nothing new
happened.

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

## State

*As of 2026-10-19.* Create, Delete, Duplicate and ReParent from the Hierarchy
go through history (#203), with Ctrl+Z, Ctrl+Y and the Edit menu. History
clears on New, Load, Play and Stop.

Inspector field edits, the gizmo, Add and Remove Component and Paste Values
still write the `World` directly, so they cannot be undone (#204). No unsaved
marker reads the step serial yet (#206).

Tests are in `modules/editor/tests/`. `History.tests.cpp` covers the cursor,
dropped redo steps, refused commands, step names, selection, the budget and id
clean-up, through a fake command. `EntityCommands.tests.cpp` runs every command
through Do, Undo and Redo against a `TestSceneOps` built on the real
`mir::scene` functions. It compares the saved scene files at each stage, rather
than component bytes, because a restored entity has new handles. The shortcuts,
the Edit menu and `AppSceneOps` itself have no unit test; the `smoke.<game>`
runs start the editor with them in place.

## Backlog

1. **Value commands for #204**: set field and set component, merged into one
   step while a widget or the gizmo is held (`TryMerge`, plus an `EndStep` the
   panel calls on release). An `EntityRef` field in their before and after
   bytes needs the same treatment as Remove's reference links, or its target
   comes back stale.
2. **Add and Remove Component** as structural commands.
3. **The unsaved marker (#206)** from the step serial and a save point.
4. **Delete and Duplicate act on one entity**, as the Hierarchy does today
   (editor.md backlog). Doing the whole selection would want one command that
   holds several records, so it stays one step.
5. **A preset that spawns entities outside the new entity's subtree** would
   leave them behind on undo, because only the subtree is captured. No preset
   does today.
6. **Pick the budget from measured snapshot sizes.** 64 MiB is a guess.

## Changed

*2026-10-19* — **Added.** Was: no undo anywhere in the engine. The Hierarchy
returned an `EntityAction` through `EditorActions`, and `App::ApplyEntityAction`
changed the scene directly. Now: `Editor` turns it into a command, `History`
runs it, and `App` only supplies `AppSceneOps`. The design that came before this
page is folded into Model and Rules.
