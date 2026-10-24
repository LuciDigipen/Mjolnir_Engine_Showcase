# app

The composition root and the frame loop. `App` is the one place that knows every
other module exists. It contains no gameplay and no subsystem of its own, and it
is not a framework — there is no registry, no service locator, and no virtual
`Game` to subclass.

`mir::app` · depends `mir::core`, `assets`, `window`, `renderer`, `editor`, `imguiruntime`, `scene`, `script`, all public; `input`, `audio`, `physics`, `serial`, `terrain` private · API `modules/app/include/app/` · maintainer Sumin Park · reviewed 2026-09-18

## Model

Fixed members and three calls: `Initialize(desc)`, then `Run()`, then
`Shutdown()`.

The member list is the initialisation order, and reverse-order destruction is
what makes teardown correct, so declaration order in `App.h` is load-bearing
rather than cosmetic. Adding a subsystem means editing `App`, which forces a
decision about where it belongs in that order.

`Run` is a plain loop with no fixed timestep.

```
Start(scheduler)                       systems cache their queries
while not ShouldClose:
    PollEvents                         close flag and size refresh here
    dt = min(elapsed, mMaxDeltaSeconds)
    TerrainRuntime::Update                   every mode: finished terrain reads are installed here only
    if IsEditing(world): imported assets, then TerrainRuntime::Refresh   the Edit safe point
    ImGuiRuntime::BeginFrame                 the NewFrame trio
    Editor::DrawLayout -> EditorActions      New/Save/Load/Play/TogglePause/Stop and Open Project acted on here
    renderer.SetImGuiDrawData(ImGuiRuntime::EndFrame())
    renderSystem.SetViewportRect(0, Editor::SceneViewportRect())
    if IsEditing(world):                     viewport 0's camera, only in Edit
        Editor::UpdateCamera(dt)
        renderSystem.SetExternalCamera(0, Editor::SceneCamera())
    else:
        renderSystem.SetViewportCamera(0, null)   falls back to the World's own Camera
    ScriptReloadWatcher::Poll
    fixedSteps = FixedTimestep::Advance(dt)  every mode; FixedUpdate systems check IsSimulating
    SystemScheduler::Update            PreUpdate, Update, FixedUpdate x fixedSteps, PostUpdate, Render
    if ToggleFullscreen just pressed: ToggleFullscreen    F11; the swapchain follows next frame
    ++frame
    halfway through mMaxFrames, if mSwitchProjectTo: RequestOpenProject
    if mMaxFrames reached: RequestClose
```

`App` contains no ImGui call and never builds a `DrawItem`. `ImGuiRuntime`
produces a draw-data pointer and the editor a rect, and `RenderSystem` does the drawing from the
`Render` phase.

A `SystemContext` is rebuilt every tick rather than stored. It bundles references
to the world and command buffer with the values that change each frame, so
keeping one around would only be a stale copy.

`App` has one game open for its whole life, a `GameProject` from
`app/GameProject.h`: the game's directory and name, its window title, the asset
id root its scene names resolve under, its scene directory and its
`project_settings.json`. `Initialize` opens `AppDesc::mProject` and loads its
scene.

Switching games is a restart. `RequestOpenProject(dir)` only checks the directory
and asks the window to close; `main.cpp` relaunches the executable with
`--game <dir>` once `Run` and `Shutdown` have returned.

```
Project > Open Project, confirmed -> App::RequestOpenProject(dir)
    NormalizeGameDir, LoadGameProject      not a game: logged, keep running
    mProjectToOpen = game, Window::RequestClose
Run returns, Shutdown                      imgui.ini written, device and window gone
main.cpp: ShutdownLog, LaunchProcess(ExecutablePath(), --game dir)
next process: the ordinary startup path
```

`LoadScene` runs the two resolve passes in `src/app/SceneResolve.cpp` after
`SceneIO` loads, which is what turns saved script and mesh names back into live
instances and GPU handles. They are private to this module: the unload, release,
load, resolve order only holds while `App` is the one calling them.

```cpp
mir::InitLog();                 // outside App: early failures must still log
mir::ClearErrorReport();        // a report on disk always belongs to this run

try
{
    mir::GameProject project = mir::ResolveGameProject("games/HelloWorld", mir::ErrorCategory::Usage);

    mir::App app;
    mir::AppDesc desc{};
    desc.mProject = project;

    try
    {
        app.Initialize(desc);   // loads the game's scene too
        app.Run();
    }
    catch (const mir::SetupError &error)
    {
        return mir::ReportFatal(error);   // while app is alive; ~App tears down after
    }

    app.Shutdown();             // idempotent; the destructor would do it too
}
catch (const mir::SetupError &error)
{
    return mir::ReportFatal(error);
}
```

Systems are registered on `app.Systems()` between `Initialize` and `Run`.

### Play, Pause, Stop

`EngineModeState` (see [core.md](core.md)) is a `World` resource; `App` is the
only thing that flips it. `PlayScene`/`StopScene`/`TogglePause` are the three
transitions, wired to the editor toolbar through `SceneMenuAction` and to the
pause menu through `GameShellAction` (see [gameshell.md](gameshell.md)). The
menu's Pause and Resume pause and resume the simulation, Restart is Stop then
Play, and Quit stops Play in the editor and closes the window without it.
Without the editor there is no Play button, so `Initialize` calls `PlayScene`
when `mEnableEditorLayout` is off: a game build plays.

### Without the editor

`app` builds with or without the editor, by the `MJOLNIR_WITH_EDITOR` CMake
option (see [build.md](../build.md#a-build-without-the-editor)). It reaches the
code as `MIR_WITH_EDITOR`, 1 or 0, a PUBLIC compile definition of `mir::app`,
and `App::kHasEditor` says which build this is. Without it, `App` holds no
`Editor`, no `AssetImportManager`, no `ContentBrowserWatcher` and no project
menu; `mEnableEditorLayout` is
forced off; the scene viewport is the whole window through the World's camera;
and nothing sets `InputState::SetUiCapture`, since there is no editor UI to
want the input. Everything else, the game UI included, is the same code.

```cpp
PlayScene()    // Edit -> Play. Captures the World first (PlaySnapshot).
StopScene()    // Play -> Edit. Stops every started script, then restores the capture.
TogglePause()  // Running <-> Paused. No-op outside Play.
```

`StopScene` calls `ScriptSystem::StopAllScripts` (giving every started script
its `OnStop`, a Unity-style cleanup hook) *before* calling
`PlaySnapshot::RestoreWorld`, since the World those scripts ran in is about to
be destroyed wholesale. It then releases the Play session's GPU resources and
resolves scripts and meshes again, same as `LoadScene` — `RestoreWorld`'s entities come from `LoadScene` under the hood,
with the same only-names-round-trip gap.

Terrain is pinned for the whole Play (terrain ADR 0018, local). `PlayScene` runs
the terrain safe point once more before the snapshot, which puts in place a newer
release that is already checked and warm; one still being checked or prewarmed
does not hold Play up, and `TerrainRuntime::BeginPlay` lets its work go until
Stop (terrain ADR 0025, local). Nothing refreshes terrain during Play, so a
release published then changes no mesh, query or collider. After `StopScene` the
viewports upload again the terrain meshes `ReleaseSceneResources` freed, and a
release published during Play is checked and prewarmed in the background, then
switched to at a later Edit frame boundary.

## API

| Type | Header | Role |
|---|---|---|
| `AppDesc` | `app/App.h` | Window size, Vulkan app name and validation, editor-layout gate, `dt` clamp, frame limit, the game to open, a scripted game switch for tests |
| `App` | `app/App.h` | Owns the subsystems, runs the loop, takes a request to open another game |
| `GameProject` | `app/GameProject.h` | `{mDir, mName, mTitle, mAssetIdRoot, mRootDir, mSceneDir, mProjectSettingsPath}` for one game directory; `mRootDir` is where it is read from (the source tree in a development build), so terrain releases are found under `TerrainReleasesRoot(mRootDir)` |

`app/GameProject.h` also declares `ReadDefaultGame`, `NormalizeGameDir`,
`LoadGameProject`, `ResolveGameProject` (normalize, then load),
`EnsureGameDirectories` and `DiscoverGameProjects` (every `games/*/game.json`
next to the executable). All but `DiscoverGameProjects` throw a `SetupError`.
The caller passes the category for a game that is not found: Usage when the name
came from `--game`, Project data when it came from `engine.json`.
`DiscoverGameProjects` skips a broken game with a logged error, so one bad
`game.json` does not hide the others from the project menu.

`app/ErrorReport.h` declares `ReportFatal`, which logs a `SetupError`, writes
`logs/last-error.json`, starts `MjolnirErrorReporter` and returns the exit code.
It also declares `ClearErrorReport` and `ErrorReportPath`.

`mAppName` stays caller-set even though each game has its own title — it is the
Vulkan instance name, engine-level, and nothing user-facing ever sees it.

Accessors: `GetWorld()`, `Systems()`, `Scripts()`, `ScriptReload()`, `Splash()`,
`Assets()`, `Scene()`, `Project()`, `AvailableProjects()` and `ProjectToOpen()`.
`Renderer()` is marked a temporary seam, because `Renderer.h` is still an empty
abstraction placeholder.

`ToggleFullscreen()` switches the window between windowed and fullscreen and
asks the renderer to recreate its swapchain at the start of the next frame. `Run`
calls it when the `ToggleFullscreen` action is just pressed, F11 by default (see
`input/DefaultInputMap.h`). The recreate is requested rather than left to the
renderer's size check, because a window already the size of its monitor goes
fullscreen without changing size. The action is read after `Update`, so the frame
that saw the key renders at the old size and the next one at the new.

Scene control is `NewScene(name)`, `SaveScene()` and `LoadScene()`, each of which
replaces whatever `Scene()` currently holds. `NewScene`, `LoadScene` and
`StopScene` release the discarded entities' GPU meshes and materials. Game
control is `RequestOpenProject(dir)`, and the switch itself is a restart.

In an editor build, the Hierarchy's create, delete, duplicate and reparent run
inside the editor, as undoable commands (see
[editor-history.md](editor-history.md)). They reach the scene through the
private `AppSceneOps`, the `ISceneOps` `App` hands to `Editor::Initialize`. It
creates with `CreateSceneEntity`, destroys with `DestroySceneEntity`, duplicates
with `DuplicateSceneEntity`, and captures and restores with `CaptureEntity` and
`RestoreEntityAt` (see [scene.md](scene.md)). Each goes through `Scene()`, so the
entity is saved or dropped with the scene. After a restore or a duplicate it
runs `ResolveSceneScripts`, because a copied or restored `ScriptRef` has no
script instance and `ScriptSystem` skips it.

`NewScene`, `LoadScene`, `PlayScene` and `StopScene` call
`Editor::ClearHistory`, since each replaces the entities that history's steps
point at. Unlike `NewScene`/`SaveScene`/`LoadScene`, the entity operations run
during Play too, and Stop discards them with the rest of Play's changes.

The editor's unsaved marker is set from here too. A successful `SaveScene`, and
`NewScene` and `LoadScene` after clearing, call `Editor::SetSceneUnsaved(false)`.
`PlayScene` remembers whether the scene was unsaved, and `StopScene` puts that
back, because `RestoreWorld` returns the `World` to the moment Play began.

### Ownership and order

| Member | Created | Destroyed | Because |
|---|---|---|---|
| `mSplash` | first | last | shown before the window exists |
| `mWindow` | early | late | the renderer's surface is built from it |
| `mRenderer` | after the window | before it | holds `VkSurfaceKHR` and the device |
| `mEditor` | after the renderer | before it | its Vulkan backend needs a live device, its GLFW backend a live window |
| `mSceneOps` (`AppSceneOps`, editor builds only) | just before `mEditor.Initialize` | with `App` | holds only a reference to `App`; the editor keeps a pointer to it |
| `mContentBrowser` (`ContentBrowserWatcher`, editor builds only) | with `App` | with `App` | owns no OS handle - a poll-based watcher only, so no explicit `Shutdown` |
| `mCommands` | before `mWorld` | after it | outlives the world it records against |
| `mWorld`, `mScheduler`, `mScriptHost`, `mScene` | with `App` | with `App` | no external resources |
| `mTerrain` (`TerrainRuntime`, private) | in `Initialize`, after `RenderSystem` | with `App` | holds handles only: its meshes go with the renderer, its height field bodies with the World's `PhysicsWorld`; it unregisters itself from the `ViewDrawSources` and `PhysicsStepGates` resources |
| `mProject`, `mAvailableProjects`, `mProjectToOpen` | with `App` | with `App` | plain data |
| `mAssetManifest` | in `Initialize`, after the editor | after the cache | owns the parsed manifest; empty when the build cooked nothing |
| `mAssetCache` | with the manifest | before it | holds a raw pointer into it |

The manifest and cache pair is the one real aliasing hazard, and it is handled
twice. Declaration order makes implicit destruction correct, and `Shutdown` also
calls `reset()` on them explicitly in the same order, because `Initialize` may
follow.

`mRenderSystem`/`mScriptSystem` are non-owning — `AddSystem<T>` returns a `T&`
into `mScheduler`'s storage, captured here so `Run` and `StopScene` can reach
them without a lookup.

### Resolving names back into handles

A `LoadedScene` file (see [scene.md](scene.md)) round-trips a `MeshRenderer`'s
`meshName`/`materialShader` and a `ScriptRef`'s `scriptName` — strings, not the
live `MeshHandle`/`instanceRef` `RenderSystem`/`ScriptSystem` actually need.
`materialShader` and a `scriptName` always turn into handles by asset id under
the game's `mAssetIdRoot`; a `meshName` does too, *unless* it's one of
`Shapes.h`'s reserved builtin names (`MakeBuiltinMesh`), in which case it
generates its geometry on the spot instead — no asset file, used by the
editor's primitive entities (see [editor.md](editor.md)).

`ResolveSceneScripts`, in the private `SceneResolve.cpp`, runs at the end of both
places entities come from a `LoadedScene` file: `LoadScene()` and `StopScene()`
(via `PlaySnapshot::RestoreWorld`). Neither call site can skip it independently,
because both live inside the same two methods.

Meshes and materials resolve in `MeshResolveSystem`, a private `PostUpdate`
system registered after `ModelSystem`. Entities also get a `MeshRenderer` outside
a load: from Duplicate, Paste, Add Component and Inspector edits. A load-time pass
would leave their handles stale or null, so this runs every frame. It remembers
the `meshName` and `materialShader` each entity was last resolved from, keyed by
`PackEntity`. It resolves again only when one of them changes. Handles and
`MeshBounds` come from caches keyed by name, so renderers naming the same mesh
share one upload.

A renderer whose names are both empty is never touched, so a handle set from code
keeps its value. Clearing `meshName` nulls the mesh and removes `MeshBounds`.
Removing the `MeshRenderer` removes `MeshBounds` too. `Reset()` empties the
caches and runs wherever `ReleaseSceneResources` does: `StopScene` and
`UnloadCurrentScene`.

`ApplyImportedAssets` calls `CommitReload` with the new `AssetCache`, after
`ModelSystem`'s. It frees the cached meshes and forgets every entity's names, so
the next `PostUpdate` resolves every renderer from the new cache, including one
whose mesh failed before the import. That runs before `Render`, so no freed mesh
is drawn. A mesh file that is not valid JSON, lacks a vertex field, or has an
index past its vertices logs an error and resolves to no mesh.

A `materialShader` whose `CreateMaterial` fails logs an error and resolves to the
renderer's error material. The mesh then draws magenta, rather than the default
grey a null handle would give it.

### Terrain (private glue)

App is the only place terrain meets the renderer and physics, since neither
module may link `terrain`. A scene holds terrain as `TerrainInstance`
components; the `TerrainWorld` World resource holds what they resolve to (see
[terrain.md](terrain.md)). `src/app/TerrainRuntime.{h,cpp}` owns everything
derived from it, shared by every instance of a release: the height field
pieces a `TerrainCollisionResidency` builds for the bodies that need them
(#128), and, shared by every viewport, the GPU meshes of the draw units the
viewports select (#125).

```
Initialize     -> TerrainRuntime::Bind      streaming settings -> TerrainStreamingSettings, TerrainStreamer resources;
                                            renderer mesh streaming; collision residency; registers itself in the
                                            ViewDrawSources and PhysicsStepGates resources
Run (any mode) -> TerrainRuntime::Update    TerrainStreamer::Update, TerrainReleaseSwitch::Update,
                                            TerrainMeshResidency::Install, TerrainCollisionResidency::Install: the
                                            install point; outside Play the bodies' collision needs too; each staged
                                            release's prewarm reported to the switch
LoadScene      -> TerrainRuntime::Load      CollectTerrainScene, ResolveTerrainWorld (Async, keepLoaded), collision
                                            instances
Run (Edit)     -> TerrainRuntime::Refresh   an edit settled 0.5 s; current pointers polled every 1 s (a newer release
                                            of a loaded asset starts a background check); a ready switch adopted
PlayScene      -> Refresh(force), BeginPlay adopt a ready switch, then Play pins what is loaded; a switch in progress
                                            waits for Stop
StopScene      -> ForgetMeshes, ReleaseSceneResources, ..., Refresh(force)
UnloadCurrentScene -> Unload                meshes forgotten, collision pieces removed, TerrainWorld emptied
FixedUpdate    -> TerrainRuntime::BeforeStep  as a PhysicsStepGate before each step: install pieces, update needs,
                                            hold or resume each dynamic body, restrict its contacts
Render         -> TerrainRuntime::Prepare   per viewport: select, SetInterest; then request every viewport's meshes
                  AppendViewDraws           that view's draws
                  AppendVolumeDraws         shadow casters at viewport 0's detail
```

`Bind` reads the game's `terrain_streaming.json` with the machine-local
`terrain_streaming.local.json` beside the executable layered over it (see
[terrain.md](terrain.md#streaming-and-cpu-residency-issue-126)). Valid settings
become the `TerrainStreamingSettings` and `TerrainStreamer` World resources, the
streamer with a `TerrainWorkerPool` that also builds meshes; the `gpu` section
configures the renderer's mesh streaming (staging ring, bytes a frame) and a
`TerrainMeshResidency`. Invalid settings, or missing ones in
a scene with terrain, are one error, and no terrain loads; a game without
terrain needs no file.

Releases resolve through the streamer with `TerrainEagerLoad::Async`, so
nothing waits at load, on Play entry or at an Edit switch, and with
`keepLoaded`, so a loaded asset keeps its release until its
`TerrainReleaseSwitch` has checked the newer one on the worker pool (index and
every pack, one at a time) and prewarmed it. Prewarm runs a second selector per
viewport over the staged release, never drawn, whose Visible units are asked
for as Prefetch, and hands the staged release to the collision residency as
staged instances, built but not placed; it is warm when those selections draw
only their chosen units and every staged piece is built. The switch then
retires the old release's meshes, moves the colliders between steps and logs
what the old release still held and the release directories kept on disk. A
failed check, mesh or piece of the staged release is one error and the loaded
release stays. The resolved
releases and placements go to the collision residency with the streamer's
mode for each: an Eager release collides as one piece over the map, an
Interest release (too big for what the budget leaves, or the game asked for
Interest) in pieces its sources ask for. Every map tile must have source LOD
and normal records in the index, or the instance is dropped with an error.

Collision sources are every dynamic `RigidBody` (position and velocity from
Jolt, radius from its shape, gravity as acceleration,
`collision.bodyToleranceMeters` as tolerance) plus whatever a game puts in the
`TerrainCollisionSources` resource, which gets each source's status back.
`TerrainRuntime` is a `PhysicsStepGate`: before every fixed step it installs
finished pieces, updates the needs with the step's `dt` and holds each body
whose answer is `Pending` or `Failed` with `PhysicsWorld::HoldBody`, resuming
it once the answer is `Ready` or `CoarseSufficient`. It logs each hold (at
spawn, after a teleport or in motion), resume and state change at INFO, with
the pieces counted and the contact error, and a body with no single owner
piece once as a warning. Then it restricts each body's contacts
(`RestrictContacts`) to the pieces its answer names. Pieces placed and retired
are logged at INFO with spacing, error, build time, bytes and, for a piece a
body waited for, the time from first need to collidable; a failed piece is an
error, and the bodies that need it stay held.

Drawing is per viewport. `RenderSystem` hands `Prepare` every viewport's camera
each frame; for each one, `TerrainRuntime` keeps a `TerrainViewSelector` and a
streamer interest (both created on the viewport's first frame and removed when
it has no camera), updates the selection over every loaded instance, replaces
the interest's wants with the selection's, then hands every viewport's wants to
the `TerrainMeshResidency` (see
[terrain.md](terrain.md#gpu-meshes-issue-127)) and turns the drawn units into
`WorldDrawItem`s. A unit counts as ready only once its GPU mesh is usable, so
the selector keeps a ready coarser or finer unit drawn until then. The
residency builds a wanted unit's mesh on the worker pool from the resident LOD
and normal payloads (through `MakeTerrainRenderMesh`) or the Region payload,
and `TerrainGpuUploader` (`src/app/TerrainRender.{h,cpp}`) queues it with the
renderer's `QueueMesh`, with a BLAS only while the project's `rayTracing` is on
and the device supports it (turning it either way retires every terrain mesh,
to be made again); meshes are keyed by payload, so instances of one release and
all viewports share them. A mesh nobody wanted or drew for
`gpu.meshRetainSeconds` is retired with `RetireMesh`, and a swap retires every
mesh of a release no longer loaded; neither waits. Each draw sits at placement
plus the mesh origin, added in double, with the diagnostic tint on the default
material and a `translationGrid` of 1/4096 m. Each viewport's selection (units
per kind and level, stand-ins, missing areas, wants, prefetch bytes, GPU meshes
ready, uploading and building, held and retiring bytes, streamer residency,
the renderer's uploaded bytes and device and queue waits) is logged when it
changes, at most every `kTerrainViewLogSeconds`.

A swap retires the meshes of releases no longer loaded (with their builds and
uploads), replaces the `TerrainWorld` value and hands the new instances to the
collision residency, which removes the colliders of placements and releases no
longer there at once; viewports pick the new release from the next frame. A
piece that cannot be built no longer drops its terrain: it is an error, and the
bodies that need it stay held. Each problem is logged once per loaded scene. A
piece is `BuildTerrainUniformSamples`'s block handed to
`PhysicsWorld::BuildHeightField` on a worker; its static body is
`CreateStaticHeightField(shape, origin)` at placement plus the block's first
sample, tagged with the instance's entity, one body per placement of a shared
shape.

## Rules

**Register systems before `Run`.** `Run` calls `SystemScheduler::Start`, and a
system added after that never gets `OnStart`.

**`Initialize` throws on any failure.** Every step that fails throws a
`SetupError` with its fix, and nothing in `Initialize` unwinds by hand. `Run`
returns immediately when `Initialize` did not finish.

**`Shutdown` is safe on a partial `Initialize`, it is idempotent, and the
destructor calls it.** Each service's own shutdown is a no-op on what it never
built, so `Shutdown` always runs in full in the same hand-written order. Calling
it explicitly is the documented style, and forgetting it is not a leak.

**Setup creates the directories a game points at.** `EnsureGameDirectories`
creates a missing assets, scene or SoundBank directory before any service reads
it.

**`project_settings.json` is checked once, at setup.** A missing file means no
settings yet. A malformed one is fatal, because each settings section falls back
to its defaults on its own and would otherwise lose them all without a word.

### Errors

| Category | Raised when |
|---|---|
| Usage | A bad command line, or a `--game` that is not a game |
| Project data | `game.json`, `engine.json`'s `defaultGame`, `project_settings.json` or the scene is broken |
| Build output | The cooked manifest is corrupt, `engine.json`, `asset_workspace.json`, AssetCooker or the fonts are missing |
| System | A game directory cannot be created or watched, or the Vulkan renderer cannot start |
| Internal | ImGui or the editor fails for a reason other than missing fonts, or an exception that is not a `SetupError` reaches `main` |

**A broken scene asks before it fails.** `ProbeScene` runs before anything is
unloaded. In an editor build, a corrupt `scene.json` offers an empty scene, and
missing entity or instance files offer to open without them. At startup No quits with the
error. From the menu No keeps the current scene. A game build never asks, and
`--non-interactive` answers No.

**Systems stop before subsystems die.** `Shutdown` runs `Stop` first, so `OnStop`
still sees a live world. The renderer and window are already on their way out by
then, so touching them from `OnStop` is not supported.

**Switching games restarts the process.** The component registry, the counter
behind every component's signature bit, the Lua VM and the asset cache are all
process-wide. Only a new process opens a game without the last one's leftovers:
script-spawned entities, Lua globals, `world:declare`d component types.
`RequestOpenProject` tears nothing down itself.

**`RequestOpenProject` checks before it closes.** A directory that is not a game
logs an error and returns `false`, and the open game keeps running. It does not
use `ResolveGameProject`, whose "Loading game" line would claim a load that
happens in the next process.

**Unloading a scene does not call `OnStop`.** New and Load destroy the scene's
entities, and `ScriptSystem`'s `ScriptRef` OnRemove hook only drops each Lua
instance.

**The game list is read once.** `DiscoverGameProjects` runs in `Initialize`. A
game added while the engine runs would still need a build to cook its assets, and
the asset manifest is never re-read.

**The asset manifest loads eagerly, and a missing one is empty.** A build with
no assets writes no manifest and is still a correct build, so `Assets()` is
never null after `Initialize`. A corrupt manifest is fatal. In an editor build
with no manifest but with source assets, `AssetImportManager` queues a full cook,
and `ApplyImportedAssets` swaps the result in once it finishes. The first frames
run without those assets. The swap resolves scripts again, and only the scripts
still unresolved load.

**`dt` is clamped, and `mElapsed` accumulates the clamped value.** After a
breakpoint or a window drag it is behind the wall clock on purpose, because it is
simulated time rather than a timestamp and must not be used as one.

The clock is `steady_clock`, so a system clock change cannot produce a negative
`dt`.

**`App` does not own logging.** `InitLog` and `FlushLog` belong to the caller, so
a construction failure inside `App` is still visible.

**`App` is neither copyable nor movable**, and it hands out references to its own
members through `SystemContext`.

`MakeContext(0.0f)` is used for both `Start` and `Stop`, since neither is a frame
and a nonzero `dt` there would be a lie. `mFrame` increments after the update, so
the first tick runs as frame 0.

**`PlayScene`/`StopScene`/`TogglePause` are the only way `EngineModeState`
changes.** `StopScene` calls `StopAllScripts` before `RestoreWorld` runs,
never after — the World those scripts ran in still has to exist for
`CallOnStop` to mean anything, and `RestoreWorld` destroys it wholesale a
moment later.

**Viewport 0's camera is mode-gated, not just fed unconditionally.**
`Editor::UpdateCamera` only runs in Edit. Feeding it in Play would let a
right-drag meant to look around the game view — viewport 0 shows the World's
camera then, not the editor one — silently fly the (invisible) editor camera
off somewhere, surfacing only later when Stop reverts viewport 0 to wherever
that ended up.

**Only `App.h` and `App.cpp` branch on `MIR_WITH_EDITOR`.** Every
editor call goes in a `#if MIR_WITH_EDITOR` block there, the editor's
per-frame work in `ApplyEditorActions`, so the game build differs from the
editor's `--no-editor` path by what is missing and nothing else. The definition
is PUBLIC because `App.h` lays out `App` differently with it: a file that saw
the other value would disagree with `App.cpp` about the class.

**`NewScene`/`SaveScene`/`LoadScene` refuse while Play is running.** These can corrupt the current scene. Both `Editor` and app will opt out.

**Terrain meshes do not survive `ReleaseSceneResources`.** It frees every mesh,
queued and retiring ones too. `TerrainRuntime::ForgetMeshes` (Stop) or `Unload`
(scene change) runs before every release, so the residency holds no handle
across it (a kept one would miss, since mesh generations move on); the next
frames upload what the viewports draw again. Dropping terrain while the scene
stays retires its meshes with `RetireMesh` (a release swap, a change of the
`rayTracing` setting, or a mesh nobody wanted or drew for
`gpu.meshRetainSeconds`), freed after their last frame without a wait. Height
fields live in the process-long `PhysicsWorld`, so `Unload` and every swap
destroy them explicitly.

**Terrain is drawn per viewport, from what is on the GPU.** Each viewport
selects its own units and wants their payloads; nothing is drawn that the
selection did not pick, a unit draws once its GPU mesh is usable, and an area
with nothing ready is left out while it loads. No terrain mesh is built or
uploaded on the frame path with a wait: builds run on the worker pool and
uploads go through `QueueMesh` within the `gpu` budgets.

**A body waits for its ground, alone.** Before every fixed step each dynamic
body whose predicted range needs terrain collision that is not ready (and has
no coarse piece within its tolerance) is held at its pose with its velocities
kept; other bodies simulate. Terrain colliders are placed and removed only in
`TerrainCollisionResidency::Install`, at the top of a frame or in the step gate,
never during a step; nothing on the frame path waits for a read or a build.

**Terrain changes only at an Edit frame boundary.** `TerrainRuntime::Refresh`
runs at the top of an Edit frame, in `PlayScene` before the snapshot and at the
end of `StopScene`, never during Play: one Play keeps the releases it entered
with, and a mesh, query or collision piece of another release never shares a
frame or a physics step with them. A staged release is selected, read, meshed and
built in Edit, but never drawn or placed, and Play drops its staging.

## State

*As of 2026-09-14.* The loop runs, the scheduler ticks, scripts drive entities,
scenes save and load from the editor's File menu, Project > Open Project restarts
into another game, and the scene renders. Play/Pause/Stop work end to end: a
Play session can be entered, its World mutations discarded on Stop via
`PlaySnapshot`, and resumed indefinitely.

The renderer is a value member rather than a `unique_ptr`, because there is no
interface to hold.

No unit tests. `App` is composition, and its behaviour is a window opening, a
device initialising and a loop running. The parts worth asserting — phase
ordering, command buffer flushing, asset lookup, scene round-tripping — are
tested where they live. A headless renderer is the prerequisite for unit testing
anything here. Until then the `smoke.<game>` tests run the whole `App` for 120
frames per game through `mMaxFrames`, and `smoke.switch` restarts from one game
into another twice through `mSwitchProjectTo`. All of them fail on a crash or a
logged error. `smoke.TerrainCheck` loads a far-world terrain from the release
the build just published, with ray tracing requested, plus a second terrain
that streams by interest; `smoke.TerrainCheck.game-mode` plays it from the
scene camera, which sees both, so the per-viewport selection draws, streams and
traces them; a sphere on each map is held until its ground is collidable and
then lands on it. The terrain state machine
is tested in `terrain` (`TerrainScene.tests.cpp`), the collider assembly in
`terrain` (`TerrainCollider.tests.cpp`), and collision residency in `terrain`
with a fake physics side and with Jolt (`TerrainCollisionResidencyJolt.tests.cpp`)
with Jolt bodies held and resumed the way `TerrainRuntime` does it; the release
switch in `terrain` (`TerrainReleaseSwitch.tests.cpp`) through the same
composition with gated reads; the Play/publish/Stop sequence through
`App` itself was checked with local harnesses (terrain ADR 0018 and 0025).

## Backlog

1. **A headless mode**, which needs the renderer abstraction. It is also the
   prerequisite for any test in this module, and for making `Editor` optional.
2. **No frame limiter**, and no window-event handling beyond the renderer
   re-querying size.
3. **`Initialize` after `Shutdown`** is written to work — the explicit resets
   exist for it — but nothing exercises it.
4. One `App` per process is assumed everywhere, and nothing enforces it.
5. **A second viewport for Play+Paused**, showing the editor camera alongside
   the game view. `RenderSystem::AddViewport` exists (see
   [renderer.md](renderer.md)); nothing calls it yet.
6. **No World diffing for Play/Stop** — `PlaySnapshot` destroys and reloads
   every entity rather than diffing what changed, which is simple but not
   cheap for a large scene.
7. **`ContentBrowserWatcher`'s Delete guard only checks the one path being
   deleted** against live `ModelPlayer` references, not every asset
   recursively under a deleted folder. A full recursive reference scan was
   left for later; the panel's confirm modal is the primary safety net.
8. **A scene opened without its missing entities is not marked unsaved.** The
   editor has no dirty flag, so the prompt's warning is the only notice that the
   next save drops them.

## Changed

*2026-10-20* — **Setup failures throw, and the asset manifest loads eagerly.**
Was: `Initialize` returned `bool` and unwound each failure by hand, a malformed
`game.json` kept its defaults, `Assets()` loaded the manifest lazily and stayed
null after a failure, and a broken scene loaded empty or partial without a word.
Now: every failure throws a `SetupError`, `Shutdown` is safe on a partial setup,
a missing manifest is an empty one, and a broken scene asks first or fails.
Because: a silent degraded start hid broken data, and the reader had no fix to
act on. The lazy manifest dates from 2026-09-01, when a missing manifest had to
cost an asset rather than the application; an empty manifest keeps that.

*2026-10-20* — **Save, New, Load, Play and Stop set the unsaved marker (#206).**
Was: `SaveScene` returned `mir::SaveScene`'s result and nothing tracked whether
the scene matched its file. Now: a successful save, New and Load mark the scene
saved, and Stop restores the marker Play found, through `Editor::SetSceneUnsaved`.

*2026-10-19* — **The editor's entity operations moved into its undo history.**
Was: the private `ApplyEntityAction` carried out each `EntityAction` the
Hierarchy returned. Now: the editor runs them as commands and reaches the scene
through the private `AppSceneOps`. A restored or duplicated entity gets its
script instance from `ResolveSceneScripts`, which a duplicate used to miss until
the next Load or Stop. New, Load, Play and Stop clear the history.

*2026-10-18* — **`MeshResolveSystem` resolves builtin primitives without
touching the asset cache.** Was: `ResolveMesh` always turned `meshName` into
an asset id under `mAssetIdRoot` and loaded it through `AssetCache` - every
mesh needed a file on disk. Now: it checks `MakeBuiltinMesh` (renderer,
`Shapes.h`) first; a reserved name (e.g. `"builtin:cube"`) generates its
`MeshData` on the spot and skips `MakeLogicalAssetPath`/`AssetCache::Load`
entirely, falling through to the existing file-backed path for anything else.
Everything downstream - the per-entity resolve memoization, `CachedMesh`
keyed by the raw name, `MeshBounds` add/update/remove, `Reset()` - is already
generic over `meshName` and needed no changes. Because: issue #214 - the
editor's new Primitive entities (see [editor.md](editor.md)) need to save and
load with the scene without an external asset file.

*2026-10-13* — **Terrain release switches checked and prewarmed in the
background (#129).** Was: the Edit poll, `PlayScene` and `StopScene` adopted a
moved `current` at once, opening its index on the main thread. Now:
`TerrainRuntime` owns a `TerrainReleaseSwitch`, resolves with `keepLoaded`,
prewarms a checked release through staged selectors and staged collision
instances, and adopts it at an Edit frame boundary; `PlayScene` calls the new
`BeginPlay`, which logs the pinned releases and lets a switch in progress wait
for Stop.

*2026-10-12* — **Terrain collision by residency, bodies held per step (#128).**
Was: `TerrainRuntime` built one height field per Eager instance at load on the
main thread, after resolving waited for every Eager release (at load, on Play
entry, at an Edit switch), dropped a terrain whose collider failed and gave
Interest instances no collider. Now: it resolves with `TerrainEagerLoad::Async`,
owns a `TerrainCollisionResidency` whose pieces build on the worker pool, and
is a `PhysicsStepGate` that holds each dynamic body until its ground is
collidable and restricts its contacts to its owner piece; it logs holds,
resumes, placed, retired and failed pieces. TerrainCheck's scene gains
`StreamedSphere` (entity 6) over the streamed map, and its
`terrain_streaming.json` the `collision` section.

*2026-10-11* — **Terrain meshes stream to the GPU without waits (#127).** Was:
`TerrainMeshCache` built a unit's mesh on the main thread and uploaded it with
the synchronous `CreateMesh` (a BLAS on any RT device) the first time it was
drawn, and destroyed unused ones with `ReleaseModelResources` (a device wait);
a unit counted as ready once its payloads were resident. Now: a
`TerrainMeshResidency` builds meshes on the worker pool and `TerrainGpuUploader`
queues them with `QueueMesh` within the new `gpu` settings, a unit is ready only
once its GPU copy is usable, a BLAS is built only while the project traces, and
retirement (unused, release swap, ray tracing toggled) is deferred to the
mesh's last frame. `view.meshRetainSeconds` moved to `gpu.meshRetainSeconds`.

*2026-10-10* — **Terrain is drawn per viewport (#125).** Was: `TerrainRuntime`
built every tile mesh of an Eager instance at load and published them all in
`TerrainDrawItems`; an Interest instance was refused. Now: it is a
`ViewDrawSource` that selects tiles and Regions per viewport
(`TerrainViewSelector`), sets each viewport's streamer interest, uploads a
unit's mesh when first drawn into the shared `TerrainMeshCache` and retires
unused ones after `view.meshRetainSeconds`; shadow passes get viewport 0's
detail. Interest instances load without a collider. TerrainCheck gains a 16 x 16
tile map at 8 m that streams by interest under a 16 MiB budget, and
`smoke.TerrainCheck.game-mode`.

*2026-10-10* — **Terrain streams through a `TerrainStreamer` (#126).** Was:
resolving a release read every pack whole and preloaded every tile on the main
thread, and tile meshes read their LOD and normal payloads synchronously. Now:
`TerrainRuntime::Bind` loads the game's streaming settings into World
resources, `Run` calls `TerrainRuntime::Update` once per frame before any
system, Eager releases are waited for at load (stop-gap until #128) and meshes
are built from resident payloads. `games/TerrainCheck/terrain_streaming.json`
is the first settings file; CMake copies a game's file next to the executable.

*2026-10-09* — **Added `ContentBrowserWatcher`.** Editor-side owner for the
new Content Browser panel (see [editor.md](editor.md)): polls the open
project's whole content root (`GameProject::mRootDir`) on a 250ms timer, the
same signature-diff idiom `AssetImportManager::PollWatcher` uses, and keeps
`ContentBrowserView` in sync with disk without an editor restart (#152). It
is a separate class from `AssetImportManager` rather than a widening of its
watcher: that one's file-type filter and 500ms cook-stability debounce exist
specifically to avoid cooking a half-written model/image, which has nothing
to do with browsing arbitrary project files, and it owns a `ChildProcess` it
must `Terminate()` in `Shutdown` where this watcher owns no OS handle at all.
`App::ApplyEditorActions` carries out its `CreateFolder`/`Rename`/`Delete`
actions, reusing the same `ModelPlayer`-reference guard already written for
`AssetImportActionKind::Remove` before a Delete that targets an asset path.

*2026-10-08* — **Meshes resolve every frame in `MeshResolveSystem`, and `App`
carries out the editor's entity actions.** Was: `ResolveSceneMeshes` ran once,
at the end of `LoadScene` and `StopScene`. A `MeshRenderer` that appeared or
changed later, from Duplicate, Paste, Add Component or an Inspector edit, kept a
stale or null handle. The editor's Create also built entities outside `Scene()`,
so Save dropped them (#96). Now: `MeshResolveSystem` replaces
`ResolveSceneMeshes` and re-resolves a renderer whenever its names change.
`ApplyEntityAction` creates, deletes and duplicates through `Scene()`.

*2026-10-08* — **A failed `materialShader` resolves to the error material.**
Was: `ResolveSceneMeshes` stored the null handle from a failed
`CreateMaterial`, so the mesh drew with the default grey material. Now: it logs
an error and stores the renderer's `ErrorMaterial()`, so the mesh draws
magenta. Because: issue #19, where a null handle came to mean "nothing
authored".

*2026-10-05* — **Fullscreen toggle.** Was: the window's mode was fixed at
creation. Now: `App::ToggleFullscreen`, called from `Run` on the
`ToggleFullscreen` action (F11), toggles the window (see
[window.md](window.md)) and calls `VulkanRenderer::RequestSwapchainRecreate`.
Because: issue #44.

*2026-10-05* — **A failed renderer initialization shuts the renderer down.**
Was: `App::Initialize` reset the window and returned when
`VulkanRenderer::Initialize` failed, so whatever the renderer had created
before the failure was never freed. Now: it calls `mRenderer.Shutdown()` first,
then resets the window, which the renderer's surface needs until its own
shutdown.

*2026-10-02* — **Scenes load terrain (#118).** Was: `TerrainRender` had no
caller. Now: the private `TerrainRuntime` resolves a scene's
`TerrainInstance`s at load, swaps meshes, height fields and the `TerrainWorld`
resource together at Edit frame boundaries, pins them for Play and rebuilds or
switches release at Stop. `GameProject` gains `mRootDir`; `App()` is defined in
`App.cpp`. New game `games/TerrainCheck/` (`smoke.TerrainCheck`).

*2026-10-02* — **Terrain draw glue (#14).** Was: app did not link `terrain`.
Now: it links it privately for `src/app/TerrainRender.{h,cpp}`, which uploads
terrain tile meshes and publishes their draws. Nothing calls it
until scenes can hold terrain (#118); `App` itself is unchanged.

*2026-10-02* — **A build without the editor.** Was: `app` linked `mir::editor`
PUBLIC unconditionally, so every binary carried the editor. Now: the
`MJOLNIR_WITH_EDITOR` option, ON by default, and the `windows-msvc-game-*`
presets that turn it off. `App` gains `kHasEditor`, `GameUIArea` and, with the
editor, `ApplyEditorActions`, which holds what `Run` did inline with
`EditorActions`.

*2026-10-02* — **The game shell, and a game build plays.** Was: `App` drew the
game UI only under `--ui-demo`, and with `--no-editor` the world stayed in Edit,
so no script ran. Now: the game UI runs every frame and draws `GameShell`, the
HUD and pause menu, in Play; `ApplyShellAction` carries out what the player
asked after the UI's frame; and `Initialize` calls `PlayScene` without the
editor. `App` still never touches `InputState::SetUiCapture` for the game UI.

*2026-09-18* — **`GameProject` reads through `mir::serial`, and owns the project
filenames.** Was: `ReadDefaultGame` and `LoadGameProject` opened their own
`std::ifstream` and parsed with `operator>>`, and `"engine.json"`, `"game.json"`
and `"project_settings.json"` were literals spread across the file. Now they call
`ReadJsonFile`, and the three names are `kEngineManifestFileName`,
`kGameManifestFileName` and `kProjectSettingsFileName` in `GameProject.h`.

`LoadGameProject` tests for `game.json` with `is_regular_file` before reading it.
`ReadJsonFile` returns `nullopt` for both a missing and a malformed file, and this
function needs them apart: a missing manifest means the directory is not a game,
while a malformed one was a game whose manifest fell back to defaults. Since
2026-10-20 a malformed one is fatal instead.

`App` no longer spells `"scene.json"` or `"manifest.blob"`. It asks
`SceneManifestPath` (see [scene.md](scene.md)) and uses `kAssetManifestFileName`
(see [assets.md](assets.md)) instead.

`SceneResolve`'s mesh parsing uses `ParseJson`, which takes the asset id as the
context for its error message, since a cooked blob has no path to report.

*2026-09-17* — **`RenderSettings` loaded alongside `InputMap`.** `Initialize`
now also emplaces a `RenderSettings` World resource, loaded from the same
`project_settings.json` (see [renderer.md](renderer.md#changed) — the file
gained a `"rendering"` section next to `InputMap`'s `"input"`).

*2026-09-15* — **Fixed timestep for `SystemPhase::FixedUpdate`.** Was: `Run`
ticked every phase once per frame with the variable `dt`, so physics-style
systems had no stable substep. Now: `Run` advances a `FixedTimestep`
(`AppDesc::mFixedDeltaSeconds`, 1/60 s, at most `mMaxFixedSteps`, 4, a frame)
and hands the substep count to `SystemScheduler::Update`, which runs
`FixedUpdate` that many times between `Update` and `PostUpdate` with the fixed
step as `dt`, flushing after each. Every other phase still runs once with the
frame `dt`, so `Render` happens exactly once per frame.

The loop owns the clock and the scheduler owns the order: the scheduler takes a
step count and keeps no time state. Leftover time carries to the next frame,
but the backlog is clamped to one capped frame, so a sustained overload becomes
slow motion instead of a growing debt. The accumulator advances in every mode;
a `FixedUpdate` system checks `IsSimulating` itself, as `ScriptSystem` does, so
Play and Unpause start without a catch-up burst.

*2026-09-14* — **The game is chosen at launch, and switching games restarts.**
Was: `AppDesc::mProjectPath` named `games/HelloWorld/game.json`, parsed by a
private `GameProject` in `App.cpp`, so opening another game meant editing
`main.cpp`. Now: `GameProject` is public in `app/GameProject.h`, `main.cpp`
picks the game from `--game` or `engine.json` and passes it as
`AppDesc::mProject`, and the resolve passes are free functions in
`src/app/SceneResolve.cpp`. Project > Open Project calls `RequestOpenProject`,
and `main.cpp` relaunches with `--game`.

An in-process switch was built first and dropped. Script-spawned entities, Lua
globals and script-declared component types all outlived it, and clearing each
one would have meant reaching into `core` for every process-wide singleton.

*2026-09-14* — **`AppDesc::mMaxFrames`.** Was: `Run` ended only when someone
closed the window, so nothing could drive the engine unattended. Now: after
`mMaxFrames` frames `Run` calls `Window::RequestClose`, the same exit as the
close button. `main.cpp`'s `--frames` sets it for the `smoke.<game>` tests.

*2026-09-12* — **`NewScene`/`SaveScene`/`LoadScene` refuse while Play is
running.** Was: nothing stopped any of the three from running mid-Play — Save
would overwrite the authored scene file with Play's live mutations, and
New/Load would desync `mPlaySnapshot` from the World it was captured from, so
a later Stop silently restored stale content instead of erroring. Now: each
checks `IsPlaying(mWorld)` first and no-ops with a logged warning; `Editor`'s
File menu also greys the same three items out for the same reason (see
[editor.md](editor.md)).

*2026-09-12* — **`AppDesc` takes a project path instead of pre-resolved
fields.** Was: `main.cpp` parsed `game.json` into a `GameProject` and copied
three fields (`mTitle`, `mSceneDir`, `mAssetsRoot`) into `AppDesc` by hand.
Now: `AppDesc::mProjectPath` names the `game.json`, and `App::Initialize`
resolves it internally into a private `GameProject` (moved from `main.cpp`)
and two private members. `main.cpp` shrank to setting one path.

Manual field-copying at the call site is exactly the shape of thing that
silently drifts — a second executable would have to re-derive the same three
fields correctly by hand, with nothing enforcing it stayed in sync. It also
sets up project switching: a future `SwitchProject(path)` re-runs the same
internal resolution `Initialize` does, rather than requiring the switcher to
re-derive and repackage fields externally.

*2026-09-12* — **`ResolveSceneMeshes`/`ResolveSceneScripts` moved from
`main.cpp` into `App`.** Was: `main.cpp` called them once, by hand, right
after its one `LoadScene()` call — so when `PlaySnapshot::RestoreWorld`
became a second caller of the same load path, it silently inherited the load
but not the resolve, and Stop left the viewport blank (null mesh handles,
unresolved script instances).

Neither function is policy that varies per game — same mechanism, every game,
parameterized by `mAssetsRoot` — so they moved to where `LoadScene()` and
`RestoreWorld()` both already live, closing the gap structurally: nothing can
call the load path without the resolve step now, because they're the same
call.

*2026-09-12* — **Play, Pause and Stop added.** `PlayScene`/`StopScene`/
`TogglePause` flip `EngineModeState` (see [core.md](core.md)), with
`PlaySnapshot` (see [scene.md](scene.md)) capturing the World before Play and
restoring it on Stop, and `ScriptSystem::StopAllScripts` giving every started
script its `OnStop` first. Wired to a toolbar in `Editor` (see
[editor.md](editor.md)).

*2026-09-12* — **Scene-viewport wiring moved from the renderer to
`RenderSystem`.** Was: `App::Run` called `renderer.SetSceneViewport(...)`
directly. Now: `mRenderSystem->SetViewportRect(0, ...)`, since `RenderSystem`
owns the viewport list (see [renderer.md](renderer.md#changed)). `App` also now
picks that same viewport's camera each frame — the editor's free-fly camera in
Edit mode, the World's default `Camera` in Play.

*2026-09-03* — **Drawing left `App::Run`.** Was: `CollectDrawInstances` and
`DrawFrame` called by hand between scheduler phases, so draw-list assembly had no
defined ordering against the systems that produce what it draws. Now:
`RenderSystem` in `SystemPhase::Render`, taking the renderer by constructor
reference. `App::Run` became poll, edit UI, poll reload, update.

*2026-09-03* — **ImGui left `App.cpp`.** Was: about 150 lines of `ImGui::*` and
`ImGui_Impl*` calls next to window and ECS orchestration, with `App.h` needing
dockspace internals just to declare a member. Now: an `Editor` member and four
calls, and `App.cpp` includes no ImGui header.
