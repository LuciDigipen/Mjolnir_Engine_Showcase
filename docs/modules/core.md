# core

The ECS and the module machinery on top of it, plus a base layer of logging and
asserts, executable-relative paths, file reading and writing, the crash handler
and the surface contract between a window and a renderer. It knows nothing about
windows, graphics APIs or assets, and has to stay that way.

`mir::core_base` · depends nothing engine-side; `glm` (public), `spdlog` (private) · API `modules/core/include/core/` · `mir::core` · depends `mir::core_base` · API `modules/core/include_ecs/core/ecs/` · maintainer Sumin Park · reviewed 2026-10-24

## Model

One folder builds two libraries, split by include root. `core_base`
(`include/`) is `log/`, `fs/`, `math/` and `platform/`: process-wide services set
up in `main`, which link only third-party code. `core` (`include_ecs/`) is
`ecs/`, built on `core_base`. Both roots use the `core/` prefix, so no include
changes, but a library that links only `core_base` cannot find an ECS header.
That is what lets `serial` and `project` stay plain libraries.

`log/` gives logging and asserts without putting a logging library in a public
header, and counts the Error and Critical lines for `main`'s shutdown check.
`fs/` resolves paths against the executable rather than the working directory,
so a copied build tree still runs, and it holds the engine's only file-reading
and file-writing primitives. `platform/` declares `ISurfaceProvider`, which lets
a renderer take a drawing surface from something without knowing that it is a
window, starts child processes that die with the engine, and holds the crash
handler.

### Modules, stages and frame slots

A module is a class whose constructor takes a `ModuleContext`, the `World` and
the `SystemScheduler`, plus the other modules it needs. It registers its parts
there and frees them in its destructor. `CoreModule` is the first: core's
components, `EngineModeState`, `ComponentFieldListeners`, `FrameCommands`, the
`RequestQueue` and `TransformPropagateSystem`.

Everything a module registers comes back as a `Registration`, a move-only token
that undoes the registration when it dies. Component hooks, stage functions,
frame-slot functions, ImGui drawers and Lua bindings all work this way.

A *stage* is a transition `App` runs: `SceneEnd`, `SceneUnload`, `SceneUnloaded`, `SceneLoad`,
`SceneSaved`, `PlayBegin`, `PlayEnd`, `ReloadPrepare`, `ReloadCommit` and `ReloadAbort`. `App` runs
`SceneUnloaded` right after each `SceneUnload`, once every holder has let go. `RunStage` calls the
stage's functions in registration order, then flushes the command buffer. A
*frame slot* is host work once per rendered frame. `FrameBegin` runs outside the
phases, before the UI and the systems, when `App` calls `RunFrameSlot`.
`BeforeRender` runs inside `Update`, once, after `LateUpdate` and before
`Render`, then flushes, so `Render` sees what it recorded; the game UI runs
there. `UpdateLoading` does not run it. The scheduler holds both lists next to
its phases.

Each loaded scene has a `SceneInstanceId`. An entity records the instance that
was active when it was created, and `SystemContext::sceneInstance` names the
scene a stage is for, so a callback running for an outgoing scene creates its
entities under that scene. Entities and resources owned by the session, id
`kSessionOwner`, outlive a scene swap. `World::PersistEntity` moves an
entity and its descendants to the session, and `EmplaceSceneResource` stores a
resource that is released with its instance. A swap sets
`SystemContext::sceneSwapped`, and modules that own session-wide state skip
their `SceneUnload` and `SceneLoad` reset when it is set.

The `RequestQueue` World resource holds the transitions someone asked for during
a frame. `App` takes them at the top of the next frame, in push order with
duplicates dropped and `Quit` last.

### Crash handler

`InstallCrashHandler`, called first in `main`, routes access violations, stack
overflows, `std::terminate`, invalid CRT parameters and pure calls to one
handler. The faulting thread only records what happened and wakes a thread the
handler started at install. That thread writes `last-error.json` and a minidump
with raw OS calls and precomputed strings, starts the error reporter, and ends
the process with exit 20.

### Engine mode

`EngineModeState`, a `World` resource, is `EngineMode{Edit, Play}` plus
`PlayState{Running, Paused}` as its own axis — Play has a running/paused
sub-state, Edit does not. Systems can check `IsSimulating(world)`/ `IsEditing`/`IsPlaying`/`IsPaused` to check if they need to run. The
raw setters (`SetEngineMode`/`SetPlayState`) only flip the flags — they carry no
World snapshot/restore of their own; see [scene.md](scene.md)'s `PlaySnapshot`
for that.

### Storage

Storage is archetype tables, and only that.

Entities with the same component set share an `Archetype`, which holds one
`ComponentColumn` per type. Adding or removing a component computes a new
signature, finds or creates the archetype for it, and moves the row. That move is
a `memcpy` per column, which is why components are POD. Removing a row swaps the
last row into the hole it left.

A component with no fields is a **tag**. It gets a signature bit and no column at
all, so `AddTag<T>` only moves the entity into the archetype carrying that bit.

### Resources

Some state belongs to a world but not to any entity — a camera, an asset cache,
a script VM. That state lives in `World`, keyed by type.

Resources are not components and carry none of the POD constraints. Nothing ever
relocates one, so a resource may own heap memory and have a destructor.

```cpp
world.EmplaceResource<ComponentFieldListeners>();
ComponentFieldListeners *listeners = world.TryResource<ComponentFieldListeners>();
```

`GroundQuery` is a World resource for consumers that cannot link terrain.
`Bind` registers an `IGroundQuerySource` through a `Registration`; `Sample`
answers at the finest source spacing, `Probe` reports coverage and source
spacing from layout metadata without loading payloads, and `Revision` moves when
the binding or the terrain behind it changes so a pending consumer can cancel.

### Access by name

Some callers have a name rather than a type — a script binding, an inspector, a
deserializer. Those go through `ComponentRegistry::Instance()`.

The registry maps a name to a `ComponentOps`, which carries get, has, add,
remove and the two deferred forms, and to a `FieldDesc` table for named values.

A script declares its own component through the same registry, and it lands in
the same archetype tables as a C++ one:

```cpp
constexpr RuntimeFieldDecl fields[] = {{"hp", FieldKind::Int}, {"speed", FieldKind::Float}};
const ComponentOps &health = ComponentRegistry::Instance().RegisterRuntime("Health", fields);
```

## API

| Type | Header | Role |
|---|---|---|
| `Entity` | `ecs/Entity.h` | `{index, generation}` handle, 8 bytes |
| `TypeId` | `ecs/TypeId.h` | Per-type `{seq, hash, name}` |
| `Signature` | `ecs/Signature.h` | 256-bit component set, the archetype key |
| `Archetype`, `ComponentColumn` | `ecs/Archetype.h` | Rows sharing a signature; one type's data in one |
| `World` | `ecs/World.h` | Owns entities, archetypes, resources and the scene graph |
| `ComponentHooks` | `ecs/World.h` | One OnAdd and one OnRemove per component type, per `World` |
| `ComponentFieldListeners`, `ComponentFieldChange` | `ecs/ComponentFieldChanges.h` | Callbacks before and after a commit writes reflected fields; which fields it wrote |
| `HierarchyIndex` | `ecs/HierarchyIndex.h` | The scene graph and the root order, read through `World::HierarchyIndex()` |
| `SetParent`, `MoveToLastSibling` | `ecs/TransformHierarchy.h` | Reparent or reorder, keeping the world pose by default; move to the end of the siblings |
| `CanSetParent`, `CanSetParentAll` | `ecs/TransformHierarchy.h` | The checks `SetParent` makes, without changing anything; for one entity or a list |
| `TopmostInHierarchyOrder` | `ecs/TransformHierarchy.h` | A list in hierarchy order, minus entries whose ancestor is also listed |
| `Transform`, `WorldTransform` | `ecs/components/` | Authored local TRS; derived world matrix plus staleness stamps |
| `DecomposeRotationScale` | `ecs/components/WorldTransform.h` | A linear matrix split into rotation and per-axis scale, shear dropped, a mirror on x |
| `Query<Ts...>` | `ecs/Query.h` | Cached view over matching archetypes |
| `RuntimeQuery` | `ecs/RuntimeQuery.h` | The same walk for terms known only at runtime |
| `ComponentRegistry`, `FieldDesc` | `ecs/ComponentRegistry.h`, `ecs/ComponentFields.h` | Name to erased ops; one named, typed value |
| `CommandBuffer` | `ecs/CommandBuffer.h` | Deferred structural changes |
| `ISystem`, `SystemScheduler` | `ecs/System.h`, `ecs/SystemScheduler.h` | Per-frame work, and running it by phase; stage and frame-slot lists |
| `SystemTies`, `SystemTiesFromEnvironment` | `ecs/SystemScheduler.h` | How equal orders are arranged: registration order, or reversed or shuffled for tests; read from `MIR_SYSTEM_TIES` |
| `Stage`, `FrameSlot` | `ecs/System.h` | The transitions App runs, and the host work slots of a frame |
| `Registration` | `Registration.h` (base) | Move-only token that undoes a registration when it dies |
| `RegistrationList<F>` | `RegistrationList.h` (base) | Functions run in registration order, each added through a token: stages, frame slots, drawers, Lua bindings, screens |
| `ModuleContext`, `CoreModule` | `ecs/ModuleContext.h`, `ecs/CoreModule.h` | What every module constructor gets; core's own module |
| `Request`, `RequestQueue` | `ecs/Requests.h` | Transitions asked for during a frame, applied at the top of the next |
| `InstallCrashHandler`, `ArmCrashHandler`, `ReclaimCrashHandler` | `platform/CrashHandler.h` | The crash handler; per-thread setup; taking the filter slot back from a library |
| `ErrorCount` | `log/Log.h` | Error and Critical lines logged so far |
| `FixedTimestep` | `ecs/FixedTimestep.h` | Frame time to `FixedUpdate` substeps: leftover carries, per-frame cap, bounded backlog |
| `ISurfaceProvider` | `platform/Surface.h` | Native handle plus dimensions |
| `ChildProcess` | `platform/Process.h` | A child process in a job object, so it dies with this one however it ends |
| `LaunchProcess`, `QuoteCommandLineArgument` | `platform/Process.h` | Start a process and optionally wait for its exit code; Windows argument quoting |
| `ExecutableDir`, `ExecutablePath` | `fs/Paths.h` | The running executable's directory and full path |
| `UserDataDir` | `fs/Paths.h` | Per-user files: `%LOCALAPPDATA%/Mjolnir` on Windows, next to the executable elsewhere |
| `ReadBytes`, `ReadFirstBytes`, `ReadText` | `fs/FileIO.h` | A whole file, its first N bytes, or its text |
| `ReadByteRange`, `FileSize` | `fs/FileIO.h` | Exactly one byte range of a file without reading the rest; a regular file's size |
| `WriteBytes`, `WriteText` | `fs/FileIO.h` | Bytes or text to a path |
| `WriteBytesAtomic`, `WriteTextAtomic` | `fs/FileIO.h` | The same, through a temp file and a rename |
| `InitLog`, `FlushLog`, `ShutdownLog` | `log/Log.h` | Logger lifetime; `ShutdownLog` releases the log file for another process |
| `EngineMode`, `PlayState`, `EngineModeState` | `ecs/EngineMode.h` | Edit/Play and Play's Running/Paused sub-state, as a `World` resource |
| `PrimaryView` | `ecs/PrimaryView.h` | World resource: the camera position, camera-relative view-projection and pixel size viewport 0 renders this frame, written by `renderer` last in `LateUpdate` and read by `gameui`, which cannot link it |

`TypeIdOf<T>()` returns three things. `seq` is process-local and is used directly
as a signature bit position. `hash` is FNV-1a of the name, stable across runs,
which is what serialisation uses. `name` is the type name itself.

That name is extracted from `__FUNCSIG__` or `__PRETTY_FUNCTION__` at compile
time and then normalised — keywords stripped, spaces collapsed, namespaces
removed — so every compiler produces the same string. Namespaces being stripped
is why component names have to be globally unique.

`GetOrCreateQuery<Ts...>(filters...)` returns a cached `Query&`, which
re-resolves its matching archetypes when `World::Generation()` changes. Filters
are `With`, `Without` and `Or`, and each `Or` is its own clause.

`RuntimeQuery` shares `detail::ArchetypeMatcher` with `Query`, so the match test
and the cache rule have one implementation between them. It hands its callback
`(Entity, std::span<void *const>)`, valid for that call only. Unlike `Query`, the
caller owns it.

## Rules

**Components are POD.** `MIR_ASSERT_COMPONENT` checks it at the declaration and
storage checks it again. Anything that owns memory belongs in a resource, with
the component holding a handle to it.

**Component names are globally unique.** A collision asserts in Debug and would
silently alias in Release. `ComponentRegistry` re-checks with `MIR_CHECK`,
because a name that comes from data can collide in a Release build too.

**No structural change during iteration.** An add moves the entity to another
table and swap-removes its old row, so an entity you have not visited yet slides
into a row you already passed and is skipped without any sign. `AddComponent`,
`RemoveComponent` and `DestroyEntity` assert if a query is walking. Use the
`CommandBuffer` instead, and call `World::IsIterating()` if you are writing a
binding layer that has to route between the two.

`CreateEntity` is exempt. A new entity lands in the empty archetype, so no table
that a walk is matching against moves.

**A component type has at most one pair of hooks, and its owner sets them.** The
owning module calls `SetComponentHooks<T>` in its constructor and keeps the
returned token as one of its last-declared members, so the hooks go before
anything they call into is destroyed. OnAdd runs after the value is stored. OnRemove runs before the data is
dropped, on `RemoveComponent` and on `DestroyEntity`, so it can read the
component it is losing. Tags fire hooks too.

An OnAdd hook may change its own entity, for example to add a companion
component. An OnRemove hook must not change its own entity, and that asserts. It
may destroy other entities, an ancestor included. When a `RemoveComponent` hook
destroys an ancestor, the cascade takes its own entity too. That destroy skips
the hook already running, so each OnRemove fires exactly once, and the removal
ends with the destroy. Neither kind may set or clear hooks, because that
could resize the hook slots or destroy the running hook. World counts the hooks
that are running, so setting hooks or dropping a hook token asserts inside one,
and in Release it logs an error and does nothing.

**Hooks own structure; `ComponentFieldListeners` owns field writes.** A commit
that writes reflected fields, today only `PrefabCoordinator`'s, notifies the
listeners once before and once after, with the fields it wrote per component.
Its adds, removes and destroys go through `World`, so they reach the owners'
hooks and are not in the payload. Inspector and undo writes notify nobody.

**`DestroyEntity` is bottom-up.** Children are destroyed fully first, last
child first. Each child stays linked until its own removal, so its hooks still
read its parent. The loop drains the live child list, so a sibling a hook
already destroyed is gone from it.
Then the entity's OnRemove hooks run in bit order, and the row drops once after
the last one, so every hook reads all of the entity's data.

**A destroy already in progress finishes its own entity.** A hook that destroys
an entity already being destroyed is ignored with a warning. A child that is
mid-destroy further up the stack is cut loose from its parent first, so its own
removal never reaches a parent that is gone. A dying entity takes no part in new
links, as a child or as a parent. `SetParent` asserts and returns `false` for
either, and `CanSetParent` answers `false`. During a drain the child list can
therefore only shrink.

A dead entity left in the graph, or a child whose destroy did not unlink it, is
a bug. Both assert in Debug. In Release the child is cut loose and the destroy
continues, so the loop always ends.

**A resource destructor must not call back into `World`.** By then `~World` is
already destroying the resource map. Anything holding entity handles needs an
explicit shutdown call rather than destructor cleanup.

**Re-emplacing a resource replaces and destroys the old one**, which invalidates
cached pointers to that resource. Emplace during setup, not mid-frame.

Other resources keep their addresses. Each one lives in its own heap holder, so
systems can safely cache a pointer in `OnStart`.

**Scene structure is not in components.** Parent and child edges live in
`HierarchyIndex`, where `RemoveComponent` cannot reach them.

`SetParent` refuses rather than asserts. It returns `false` on a cycle, on
self-parenting, on a chain that would pass `kMaxHierarchyDepth`, and on an
anchor that is not a sibling under the new parent. Asserts compile out under
`NDEBUG`, and a single cycle makes every upward walk in the engine
non-terminating, so the graph has to stay bounded and acyclic in every build.

`CanSetParent` runs the same checks without changing anything.
`HierarchyIndex::SetParent` calls it first, so the editor's drop highlight
matches the graph checks. The keep-world position fit is checked only when the
move is applied, so a highlighted drop can still be refused.

**Every entity has a place in an ordered list.** A child sits in its parent's
children, and a root sits in `HierarchyIndex::Roots()`. `CreateEntity` appends
each new entity to the roots, so roots keep creation order. The graph is a
`World` member, so every entity is in it from create to destroy.

`SetParent(world, child, parent, before)` inserts the child in front of the
sibling `before`, or last when `before` is null. A null `parent` uses the root
list the same way. The same parent with a null anchor is a no-op, as in Unity,
so a redundant call never reorders. Moving to the end of the current parent is
`MoveToLastSibling`, Unity's `SetAsLastSibling`.

**`SetParent` keeps the world pose by default.** With `keepWorld` on, it
computes the local values that hold the child's world position, rotation and
scale under the new parent. It computes them before changing the graph, and
writes them through the `Transform` setters only once the graph accepts the
move. A move whose position does not fit under the new parent is refused
whole.

The rotation and scale come from `DecomposeRotationScale` over
`inverse(parentLinear) * childWorldLinear`. A rotated parent with non-uniform
scale produces shear, which TRS cannot hold, so the shear is dropped. A mirror
is kept as a negative x scale, so the rotation stays a unit quaternion. A parent
axis shorter than `1e-12` makes the parent count as translation only, which
keeps `glm::inverse`'s determinant clear of float underflow.

A reorder under the same parent skips the recompute, so local values never
drift from a round trip. An entity without a `Transform` is reparented in the
graph only. Callers whose values are already parent-local pass `keepWorld =
false`: `AddTransform`, scene load and snapshot restore.

Destroying an entity destroys its subtree. That is the only policy — detaching a
subtree instead is not expressible today.

**`Transform` is written through its setters.** Each setter bumps a version that
`WorldTransform` compares against, so a raw write leaves every descendant stale
permanently. That is why the members are private, and why reflection over raw
field offsets does not work on this type.

`ResolveWorld` refreshes an entity and every stale ancestor before returning, so
a read is correct at any point in the frame.
`TransformPropagateSystem` resolves everything once in `Propagate`, which is a
batching optimisation rather than the correctness mechanism.

The cost is that `ResolveWorld` mutates on read, so it is neither `const` nor
thread-safe.

**At most 256 component types**, counting C++ and script-declared ones together.
The budget is charged per distinct name for the life of the process, so a script
reload costs nothing.

Overflow stops the process in every build, checked once at allocation and again
on use. The second check is what catches a C++ component that was never
registered and drew its `seq` straight from `TypeIdOf`. As an assert it would
reach `std::bitset::set` in Release and throw out of `World::AddComponent`, into
callers that do not catch it.

**Erased operations are total.** `ComponentOps` answers for a dead entity instead
of asserting. The typed API asserts because touching a destroyed entity is a bug
in engine code, but a script holding a handle across the frame that destroyed it
is ordinary.

**A component is reachable by name only once registered**, and registration is
explicit rather than a static initialiser. `engine_core` is a static library, so
a self-registering object in a translation unit that nothing references gets
dropped by the linker, and the component would be present in Debug and missing in
Release.

**`FieldDesc` uses accessor thunks, not offsets**, for any component with an
invariant. An offset write to `mPosition` moves the object and leaves `mVersion`
untouched, which produces no crash and no assert, just one wrong frame in
everything downstream. Script-declared components have no invariant to protect
and are offset-backed.

**A stale `Entity` is detected, not honoured.** `GetComponent` returns `nullptr`
for a dead handle. `AddComponent` and the rest assert.

**Systems register before `Start`.** `Start` flushes at every phase boundary, so
an entity spawned in one `OnStart` is visible to the next phase's `OnStart`.
`OnStop` runs in the same order as `OnStart`, so scripts stop while the runtimes
they call still run.

**Only `TransformPropagateSystem` runs in `Propagate`.** `AddSystem` asserts on
anything else there. Transform writers go before it, in `PostUpdate`, and
`WorldTransform` readers after it, in `LateUpdate`.

**Inside a phase, systems run by `SystemOrder`.** `AddSystem` takes the phase and
an order from 0 to 100, with the borders `kEarly` (10), `kNormal` (50) and `kLate`
(90). The lowest order runs first, and `Start`, `Update` and `Stop` all use that
order. Equal orders are unspecified, so a system that needs another first takes a
different order.

`MIR_SYSTEM_TIES` checks that nothing relies on a tie. `reverse` runs every set
of equal orders in reverse registration order and `shuffle` in a seeded random
one, whose seed the first scheduler logs; `shuffle:<seed>` repeats it. A test
executable has no log, so there the line goes to stderr. Every test executable
runs whole under both, as `<target>.ties-reverse` and `<target>.ties-shuffle`,
except the suites that never run a scheduler and say `NO_SYSTEM_TIES`
(`core_base`, `terrain`). In Debug, `Start` logs each phase's resolved order, one
line per phase.

These are the constraints the orders carry:

| Phase | Earlier | Later | Why |
|---|---|---|---|
| `PreUpdate` | `PrefabSystem` (`kEarly - 5`) | `ScriptResolveSystem`, `ScriptSystem` | It commits at the Edit safe point, so a `ScriptRef` a publish adds resolves and runs the same frame |
| `PreUpdate` | `InputSystem` (`kEarly`) | `ScriptSystem` (`kNormal`) | Scripts read this frame's `InputState` |
| `PreUpdate` | `ScriptResolveSystem` (`kEarly`) | `ScriptSystem` (`kNormal`) | A `ScriptRef` made last frame runs this frame |
| `FixedUpdate` | `ScriptFixedSystem` (`kEarly`) | `PhysicsSystem` (`kNormal`) | What `OnFixedUpdate` sets acts on the same step |
| `LateUpdate` | `CameraControllerSystem` (`kEarly`) | `AudioListenerSystem`, `AudioEmitterSystem` (`kNormal`) | It moves the camera after `Propagate` and refreshes its `WorldTransform`, which a listener on the camera reads |
| `LateUpdate` | `AudioEmitterSystem`, `AudioListenerSystem`, `SoundtrackSystem` (`kNormal`) | `AudioRenderSystem` (`kLate`) | The render sends Wwise this frame's positions |
| `LateUpdate` | `ParticleUpdateSystem` (70) | `ParticleSubmitSystem` (80) | Submit uploads what the update simulated this frame |
| `LateUpdate` | the rest of the phase | `ActiveCameraSystem` (`kMax`) | The game UI in `BeforeRender` and `RenderSystem` see the camera picked this frame |
| `PostUpdate`, in a prefab preview's own scheduler | `TransformPropagateSystem` (`kEarly`) | `ModelSystem` (`kNormal`) | `ModelSystem` reads the preview's `WorldTransform` |

`physics_script`'s `SystemOrder.tests.cpp` pins the `InputSystem`,
`ScriptResolveSystem` and `ScriptFixedSystem` rows by building the input, script
and physics modules. No test target links the modules of the other rows
together, or they need Vulkan or Wwise to build.

**A registration list changes only between runs.** Every `RegistrationList`
(stages, frame slots, ImGui drawers, Lua bindings, game UI screens) asserts on
an add or a removal while it runs, because either could move the running
function. A function that throws stops the run and reaches the caller, which is
how a `ReloadPrepare` function vetoes a reload. A list asserts in its destructor
if a registration is still alive, so a token outliving its list is caught where
it happens.

**A system's destructor touches nothing outside the system.** Systems belong to
the scheduler, which `App` destroys after every module, so a system outlives the
runtimes its module owns. Cleanup that reaches Jolt, Wwise, the Lua host or the
renderer goes in `OnStop`, which `~App` runs while every module is alive, or in
the owning module's destructor.

**A token dies before its registry.** A token asserts when the `World`,
scheduler or host it came from is already gone. A module keeps its tokens as its
last-declared members, because the destructor body runs with every member
alive and then members die in reverse.

**A C callback or thread entry is `noexcept`.** Its body may fail only by
allocation, which terminates and reaches the crash handler. An engine-owned
thread calls `ArmCrashHandler` first: MSVC keeps the terminate handler per
thread, and the call also reserves stack for the handler. A library thread with
no init hook still reaches the handler through `abort`, but two of those
terminating at once can lose the race.

**`World` is neither copyable nor movable.** Archetypes hold pointers back into
it.

**`ForEachAliveEntity` walks every live entity, not a query's matched rows.**
It exists for whole-World operations that cannot assume anything about which
components are present — `PlaySnapshot`'s capture/restore ([scene.md](scene.md))
is the one caller. A `Query` would miss any entity outside the types it was
built for; this walks `EntityPool` directly instead.

**Every file read and write in the engine goes through `fs/FileIO.h`.** No
module opens its own `std::ifstream` or `std::ofstream`. One implementation means
one place where a failed open, an unknown size and a short read are handled, and
one place that logs which path failed.

`ReadBytes` and `ReadFirstBytes` differ in more than how much they read.
`ReadBytes` accepts any size, including zero, and logs a failure. `ReadFirstBytes`
treats a file shorter than the requested count as a failure and stays silent,
because its caller is probing a fixed-size header on a file that may legitimately
not exist yet — `AssetCooker` reads 32 bytes to decide whether a cooked blob is
stale rather than reading the whole asset.

`ReadByteRange` is for files read in pieces, such as terrain tile packs. It
returns exactly `[offset, offset + length)` or nothing, and logs a missing file,
a range past the end, or an end that overflows; it never returns a short
buffer. `FileSize` fails, and logs, for anything that is not a regular file,
because MSVC's `file_size` reports a size for a directory.

**A failure is logged once, by the layer that knows why.** `FileIO` names the
path and the reason, so a caller adds a line only for something that layer cannot
know. `ReadSpirv` logs the word count it rejected but says nothing about a file it
could not open, and `LoadScene` reports that it is loading an empty scene rather
than repeating why the manifest failed to parse.

`ReadText` and `WriteText` open in binary mode. Text mode on Windows turns a
newline into a carriage-return pair on the way out and back on the way in, which
makes `file_size` disagree with what was written and makes the same file differ
byte for byte between platforms.

**Atomic writes are for content a person authored.** `WriteBytesAtomic` writes a
sibling temp file and renames it over the target, so a reader sees either the
whole old file or the whole new one. Settings and scenes use it. Cooked output
uses plain `WriteBytes`, because a tool regenerates it.

The temp file is a sibling rather than something in the system temp directory,
since a rename is only atomic within one volume. Atomic here means no torn file
is ever visible; it does not mean the write survives a power cut, because the
flush reaches the operating system rather than the disk.

**Logging is initialised in `main`, outside `App`**, so a construction failure
inside `App` is still visible. `Trace` and `Debug` compile out in Release and are
free to leave in hot paths.

## State

*As of 2026-10-24.* Working: entities, archetype storage with tags, filtered and
runtime queries, deferred structural change, per-type component hooks behind
tokens, phase scheduling, stages, the `FrameBegin` and `BeforeRender` slots, the request queue, the
crash handler, typed resources,
the transform hierarchy with lazy world-matrix resolution, ordered siblings and
roots, keep-world reparenting, the component registry
with named field access, script-declared component types, and the file-byte
primitives in `fs/FileIO.h`.

Tests in `modules/core/tests/`, two executables. `engine_core_base_tests`:
`Paths`, `FileIO`, `Process`, `LogHistory`, `SetupError`, `WorldCoordinates`,
`CrashHandler` (each crash in a `CrashProbe` child process). `engine_core_tests`:
`Archetype`, `Query`, `CommandBuffer`, `System`, `Stages`, `Requests`,
`ModuleUnwind`, `Resources`, `Transform`, `TransformHierarchy`,
`ComponentRegistry`, `ComponentHooks`, `RuntimeQuery` and the rest.

Logging and asserts have no tests of their own. Every other test exercises them,
and their failure mode is loud.

`EngineModeState` and `ForEachAliveEntity` have no tests in this module. The
former is flag-flipping with nothing to assert on in isolation; the latter is
exercised by `scene`'s `PlaySnapshot.tests.cpp`, its one real caller.

Missing: parallelism, dependency ordering, change detection, observers (several
listeners for one component event), and row migration when a script component's
layout changes.

**A request queue can be held.** `Hold` takes what is pending and `Restore` drops
everything pushed since and brings the held requests back, so a span such as a scene
load can discard what was asked during it without losing what was asked before.

**A scene load is ready when its resources are, not when assets are idle.**
`SceneLoadGroup` is a World resource that `CoreModule` restarts at every
`SceneLoad`. Each frame the systems that prepare resources report how many
bindings the scene requires and how many settled, `MeshResolveSystem` per mesh
name and `ModelSystem` per model, where settled means a published resource or a
failure that will not change. `EndFrame` decides after the frame: the first frame
after `Begin` only learns the work, and the group is ready once a full report
shows nothing unsettled. Progress never decreases, so a dependency closure that
grows does not walk the bar back. It stays ready until the next load, it ignores
global asset activity, and a World with no group, such as a preview, is never
held. A World that never began reads as ready. A system that prepares resources
overrides `ISystem::OnLoadingUpdate`, and `SystemScheduler::UpdateLoading` runs
those alone, in phase order, so `App` can prepare a scene before any gameplay
system has started.

## Backlog

1. **Row migration for a changed script component.** Changing a script
   component's field list needs the world restarted today, and the error message
   says so. The fix is to rebuild each holding archetype's column field by field,
   matching on name.
2. **Change detection.** Nothing tells a renderer which transforms moved this
   frame, though `WorldTransform::Version` already carries the information.
3. **Declared access for resources.** A parallel scheduler needs each system's
   read and write set. Resources are invisible to the query machinery that would
   otherwise report it, so either resources gain declared access, or systems that
   touch them stay serial.
4. **Reparenting is O(siblings)**, because stable order rules out swap-and-pop
   erase. The root list is one list of every root, so a scene load that parents
   N entities costs O(N²) in the worst case. Tombstones plus a stored index would
   make it O(1). Waiting on a profile.
5. Undecided: whether systems should be forbidden from calling
   `World::AddComponent` directly, rather than it being convention.

## Changed

*2026-11-22* — **A `PrimaryView` resource.** Was: the view a frame renders
lived inside `renderer`. Now: `renderer` writes it here. Because: `gameui`
projects world points through it and does not depend on `renderer`.

*2026-11-22* — **A `BeforeRender` frame slot.** Was: `FrameBegin` was the only
slot, and ran outside `Update`. Now: `BeforeRender` runs inside `Update`
between `LateUpdate` and `Render`, and `ActiveCameraSystem` moved from the
start of `Render` to the end of `LateUpdate`. Because: the game UI runs there,
so it reads this frame's transforms and camera, which it could not do before
the systems.

*2026-11-01* — **Scene readiness (`SceneLoadGroup`).** Was: a standalone game
started simulating the frame the scene loaded, with meshes and models still
queued, so they appeared as they finished. Now: resource systems report their
bindings and `App` prepares them before it starts a standalone game's gameplay, see `app`.

*2026-10-31* — **Equal orders can be reversed or shuffled (#310).** Was: equal
orders ran in registration order, and nothing checked that no system relied on
it. One did: `AudioListenerSystem` read a controlled camera's pose after
`CameraControllerSystem` moved it only because `CameraModule` is built before
`AudioModule`. Now: `MIR_SYSTEM_TIES` reverses or shuffles ties, every suite runs
under both, Debug logs each phase's order at `Start`, the Rules list the real
constraints, and `CameraControllerSystem` runs at `kEarly`. Because: a constraint
nobody wrote down breaks the first time module order changes.

*2026-10-31* — **`EntityChangeListeners` is `ComponentFieldListeners`.** Was:
one payload of removed and added entities and components plus changed fields,
in `EntityChanges.h`, with a `BeforeRemove` phase. `ScriptSystem` and
`MeshResolveSystem` released bindings from it on removal as well as on a
renamed script, mesh or shader. Now: `ComponentFieldChanges.h` carries only
`ComponentFieldChange`s, the phases are `BeforeChange` and `AfterChange`, and
removal cleanup is the `ScriptRef` and `MeshRenderer` OnRemove hooks alone.
Because: the hooks already fire on every remove and destroy the coordinator
makes, so two mechanisms did the same job.

*2026-10-25* — **`FieldDesc::Constrain` (#219).** Was: a field's enum names and
clamp were read only by the Inspector's widgets. Now: `Constrain` applies them
to a value about to be written, refusing an enum index with no name or a NaN and
clamping a clamped `Float`; `script`'s `world:set` and `world:add` call it.
Because: a script could write values the Inspector never lets through. `Write`
stays unchecked, so scene load and other C++ writers are unchanged.

*2026-10-25* — **Systems run by `SystemOrder` inside a phase.** Was: systems in
one phase ran in registration order, and a few relied on it (the script resolver
before `ScriptSystem`, the preview's propagate before its resolvers). Now:
`AddSystem` takes a `SystemOrder` from 0 to 100 after the phase, and equal orders
are unspecified. Because: registration order is module construction order, which
nothing enforces, and the script fixed-step callback has to run before physics.

*2026-10-24* — **Modules, stages and tokens; `core` split in two.** Was: one
`core` library, systems set their component hooks in `OnStart` and cleared them
in `OnStop`, `OnStop` ran in reverse, transforms propagated in `PostUpdate`, and
`App` ran every transition by hand. Now: `core_base` and `core` by include root,
`Registration` tokens for every registration, stages and the `FrameBegin` slot
in the scheduler, the `RequestQueue`, `Propagate` and `LateUpdate` phases,
`OnStop` forward, and a crash handler with a minidump. Because: modules register
their own parts, a hook must not outlive its owner, and scripts' `OnStop` ran
after the audio and physics they call had stopped.

*2026-10-24* — **Per-type component hooks replace entity hooks; `World` owns the
scene graph.** Was: `World` ran create and destroy hooks per entity, and
`RemoveComponent` ran none. Removing a `RigidBody`, `AudioEmitter` or
`ScriptRef` therefore leaked its runtime object (#222). `HierarchyIndex` was a
resource that `InstallHierarchy` added, so every reader null-checked it. Now:
each component type has one OnAdd and one OnRemove per `World`, set by its owner.
`World` holds the graph as a member and updates it on create and destroy.
Because: cleanup belongs to the component's owner, and every entity is in the
graph anyway.

*2026-10-09* — **`SetParent` keeps the world pose and takes a position.** Was:
`SetParent` only appended and left the local `Transform` alone, so a reparent
moved the entity in the world, and roots had no stored order. The Hierarchy
panel drew roots in slot order, which slot reuse scrambled. Now: `keepWorld`
defaults to on, an anchor sets the position, and `HierarchyIndex::Roots()` holds
root order, fed by the new `World::AddCreateHook`. The editor's drag-to-reparent
needed all three.

*2026-10-01* — **`ReadByteRange` and `FileSize` added.** Was: a file could be
read whole or by its first N bytes only. Now: one exact byte range and a file's
size, so the terrain release reader (#112) fetches one tile from a pack without
reading the pack, still through the one file-byte layer.

*2026-09-18* — **`fs/FileIO.h` added; `core` owns file bytes.** Was: the byte
primitives lived in `assets/AssetFileIo.h`, so `input` and `renderer` linked
`mir::assets` for the sole purpose of reading a file, and `scene`, `app` and the
renderer's SPIR-V loader opened their own streams besides. Now: `ReadBytes`,
`ReadFirstBytes`, `ReadText`, `WriteBytes`, `WriteText` and the two atomic
writers live here, and `std::ifstream` appears nowhere in the engine outside
`FileIO.cpp`.

The functions lost their `File` infix in the move — `ReadFileBytes` became
`ReadBytes` — and `ReadFilePrefix` became `ReadFirstBytes`, because `ReadPrefix`
read as though it took a path prefix rather than a byte count.

Interpreting those bytes is deliberately not here. JSON went to
[serial.md](serial.md) instead, for the reason that document's Model gives.

*2026-09-14* — **Starting processes, and handing the log file over.** Was: `core`
could say where the executable lives but not start another one, and the log file
stayed open until the process exited. Now: `ExecutablePath`,
`LaunchProcess` with Windows argument quoting in `platform/Process.h`, and
`ShutdownLog`. The editor's Open Project restarts into another game with them,
because the component registry and the type-id counter are process-wide and a
new process is the only clean slate.

*2026-09-12* — **`EngineModeState` and `World::ForEachAliveEntity` added.** Was:
no notion of Edit vs. Play in the ECS itself, and no way to enumerate every
entity independent of a query's component filter. Now: `EngineMode.h`'s
resource plus free-function readers, and `ForEachAliveEntity`, both driven by
the Play/Pause feature — a Play session needs to know it's running, and
stopping one needs to destroy every entity Play touched, including ones no
`LoadedScene` or query happens to track.

*2026-09-05* — **Sparse sets dropped; a fieldless component is a tag.** Was: two
storage models, opted into per type with `MIR_COMPONENT_SPARSE`. Now: archetype
tables only, with `kIsTagComponent<T> = std::is_empty_v<T>`.

Sparse was invisible to the typed API and refused by six erased paths, and the
erased surface is the one being grown. It had no callers outside tests. Its
intended use was empty tags on few entities, which is the case it handles worst:
with a signature bit, `Query<Transform>().With<Player>()` rejects a 99,995-row
table outright, and a sparse component owns no bit, so nothing can be rejected.
Removing it made `Or` accept a tag and let scripts declare tags, neither of which
needed new code.

Accepted knowingly: tags multiply archetype count, approaching 2^T for T
independently combining tags.

*2026-09-02* — **Scene structure moved out of components into a resource.** Was:
an intrusive POD chain, `Hierarchy{parent, firstChild, nextSibling, prevSibling}`.
Now: `HierarchyIndex`, a `World` resource with private mutators.

The chain was correct in isolation and failed repeatedly in place.
`RemoveComponent<Hierarchy>` on a mid-chain node truncated the parent's child
walk and dropped every later sibling silently, and no hook can intercept a
template that deletes invariants it has never heard of. The redundancy was never
the problem; storing it where the generic ECS could independently mutate it was.

*2026-09-02* — **`kMaxComponentTypes` raised from 128 to 256**, when scripts
gained the ability to declare component types and the set stopped being something
an author could count. It stays a static bitset: `Signature` is a by-value key
copied on every archetype transition, so heap-allocating it would cost far more
than the extra 16 bytes.
