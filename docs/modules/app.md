# app

Builds every module, runs the frame loop and the scene and Play transitions.
`App` is the one place that knows every other module exists. It contains no
gameplay and no subsystem of its own: what a library needs at startup, per frame
or at a transition, its module registers.

`mir::app` · depends every module, public, because `App.h` holds them as members; `prefab`, `serial` private · API `modules/app/include/app/` · maintainer Sumin Park · reviewed 2026-10-24

## Model

`App(desc)` builds everything, `Run()` loops until the window closes, and the
destructor stops the systems and lets the members die.

The members are the modules, declared in dependency order: a module after every
module it uses, and a bridge after `script` and its library. C++ builds them in
that order and destroys them in reverse, so a constructor that throws unwinds
only what was built. Within a system phase, systems run in the same order.
Adding a library means adding one member, which forces a decision about where
it belongs.

```
mDesc, mProject          the checks before anything is built: game directories, project_settings.json
mSplash                  shown until main closes it
mCommands, mWorld, mScheduler
CoreModule, AssetsModule, SceneModule, PrefabModule
GameUIModule, GameShellModule    before ImGui, whose shutdown frees their textures' GPU side
WindowModule, RendererModule, InputModule, AnimationModule, ImGuiModule, EditorModule (editor builds)
CameraModule, AudioModule, PhysicsModule, TerrainModule
ScriptModule, the five script bridges, TerrainRuntimeModule, GameUIImGuiModule
```

Between two module members a `SplashStep` sets the splash's status line, so the
splash follows startup without the constructor body doing it.

`Run` is a plain loop with no fixed timestep.

```
Start(scheduler)                       systems cache their queries
while not ShouldClose:
    PollEvents                         close flag and size refresh here
    dt = min(elapsed, mMaxDeltaSeconds)
    ApplyRequests                      last frame's transitions, and a game to switch to
    RunFrameSlot(FrameBegin)           script reload poll, terrain installs and Edit refresh, import and content browser polls
    ImGuiModule::RunUI                 Tools drawers (the editor), then Game drawers (the game UI)
    fixedSteps = FixedTimestep::Advance(dt)  every mode; FixedUpdate systems check IsSimulating
    SystemScheduler::Update            PreUpdate, Update, FixedUpdate x fixedSteps, PostUpdate, Propagate, LateUpdate, Render
    if ToggleFullscreen just pressed: ToggleFullscreen    F11; the swapchain follows next frame
    ++frame
    halfway through mMaxFrames, if mSwitchProjectTo: RequestOpenProject
    if mMaxFrames reached: RequestClose
```

`App` contains no ImGui call and never builds a `DrawItem`. The editor and the
game shell ask for transitions through the `RequestQueue` World resource, and
`ApplyRequests` runs them at the top of the next frame, where nothing is drawn
or running.

A `SystemContext` is rebuilt every tick rather than stored. It bundles references
to the world and command buffer with the values that change each frame, so
keeping one around would only be a stale copy.

`App` has one game open for its whole life, a `GameProject` (see
[project.md](project.md)) whose asset id root `main` resolved through `assets`.

Switching games is a restart. `RequestOpenProject(dir)` only checks the directory
and asks the window to close; `main.cpp` relaunches the executable with
`--game <dir>` once `Run` has returned and `App` is destroyed. The editor asks
for it through `RequestQueue::PushOpenProject`.

```
Project > Open Project, confirmed -> RequestQueue::PushOpenProject(dir)
next frame: App::RequestOpenProject(dir)
    NormalizeGameDir, LoadGameProject, ResolveAssetIdRoot   not a game: logged, keep running
    mProjectToOpen = game, Window::RequestClose
Run returns, App destroyed                 imgui.ini written, device and window gone
main.cpp: ShutdownLog, LaunchProcess(ExecutablePath(), --game dir)
next process: the ordinary startup path
```

`main` holds `App` in a `std::optional` declared outside its `try`, so a failure
in `Run` reports with `App` alive, and a failure while building it reports after
what was built is torn down.

### Transitions

`App` keeps what defines a transition: the scene commands, the Play snapshot and
`EngineModeState`. Every other step is a stage function a module registered.

```
NewScene     SceneUnload stage, SceneModule::New, SceneLoad stage
LoadScene    probe and prompt, SceneUnload, SceneModule::LoadFromDisk, SceneLoad
SaveScene    SceneModule::Save, SceneSaved stage
PlayScene    SceneModule::CaptureForPlay, PlayBegin stage, Play mode
StopScene    PlayEnd stage, SceneModule::RestoreFromPlay, Edit mode, SceneLoad stage
asset swap   AssetsModule::BeginReload, ReloadPrepare stage, CommitReload, ReloadCommit stage
```

`StopScene` runs `PlayEnd` before the restore, so scripts get their `OnStop` in
the World they ran in. Stop keeps the GPU caches: the restore fires `OnRemove`
for every Play entity, which drops its per-entity state, and the caches are
keyed by asset. A `ReloadPrepare` function vetoes the swap by throwing; `App`
then aborts the reload and tells the import why.

`EngineModeState` (see [core.md](core.md)) is a `World` resource; `App` is the
only thing that flips it. The editor's toolbar and the pause menu push `Play`,
`Stop`, `Pause` and `Resume` requests. The menu's Restart is Stop then Play, and
Quit stops Play in the editor and closes the window without it. Without the
editor there is no Play button, so the constructor calls `PlayScene` when
`mEnableEditorLayout` is off: a game build plays.

### Without the editor

`app` builds with or without the editor, by the `MJOLNIR_WITH_EDITOR` CMake
option (see [build.md](../build.md#a-build-without-the-editor)). It reaches the
code as `MIR_WITH_EDITOR`, 1 or 0, a PUBLIC compile definition of `mir::app`,
and `App::kHasEditor` says which build this is. Without it, `App` holds no
`EditorModule`, `mEnableEditorLayout` is forced off, and the scene viewport is
the whole window through the World's camera. Everything else, the game UI
included, is the same code.

## API

| Type | Header | Role |
|---|---|---|
| `AppDesc` | `app/App.h` | Window size, Vulkan app name and validation, editor-layout gate, `dt` clamp, frame limit, the game to open, a scripted game switch for tests |
| `App` | `app/App.h` | Builds the modules, runs the loop, runs the transitions, takes a request to open another game |

`app/ErrorReport.h` declares `ReportFatal`, which logs a `SetupError`, writes
`logs/last-error.json`, starts `MjolnirErrorReporter` and returns the exit code.
It also declares `ClearErrorReport`, `ErrorReportPath`, `CrashDumpPath` and
`ReporterPath`, which `main` hands to the crash handler too.

`mAppName` stays caller-set even though each game has its own title — it is the
Vulkan instance name, engine-level, and nothing user-facing ever sees it.

Accessors: `GetWorld()`, `Systems()`, `Splash()`, `Assets()`, `Scene()`,
`Project()` and `ProjectToOpen()`. `Renderer()` is marked a temporary seam,
because `Renderer.h` is still an empty abstraction placeholder.

`ToggleFullscreen()` switches the window between windowed and fullscreen and
asks the renderer to recreate its swapchain at the start of the next frame. `Run`
calls it when the `ToggleFullscreen` action is just pressed, F11 by default (see
`input/DefaultInputMap.h`). The recreate is requested rather than left to the
renderer's size check, because a window already the size of its monitor goes
fullscreen without changing size.

Scene control is `NewScene(name)`, `SaveScene()` and `LoadScene()`, each of which
replaces whatever `Scene()` currently holds. Game control is
`RequestOpenProject(dir)`, and the switch itself is a restart.

## Rules

**A module's startup is its constructor.** Every step that fails throws a
`SetupError` with its fix, and nothing in `App` unwinds by hand.

**Teardown is the destructor.** `~App` runs `SystemScheduler::Stop` while every
module is alive, so `OnStop` still sees a live world, then `Reset`, then the
members die in reverse. A module destructor logs what fails and carries on, and
`main` exits with 15 when anything logged an error during teardown.

**Setup creates the directories a game points at.** `EnsureGameDirectories`
creates a missing assets, scene or SoundBank directory before any module reads
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
| System | A game directory cannot be created or watched, the Vulkan renderer or the crash handler cannot start |
| Internal | ImGui or the editor fails for a reason other than missing fonts, or an exception that is not a `SetupError` reaches `main` |

**A broken scene asks before it fails.** `ProbeScene` runs before anything is
unloaded. In an editor build, a corrupt `scene.json` offers an empty scene, and
missing entity or instance files offer to open without them. At startup No quits with the
error. From the menu No keeps the current scene. A game build never asks, and
`--non-interactive` answers No. A scene opened either way is incomplete in
`SceneModule::LastLoad()`, so the editor marks it unsaved.

**Switching games restarts the process.** The component registry, the counter
behind every component's signature bit, the Lua VM and the asset cache are all
process-wide. Only a new process opens a game without the last one's leftovers:
script-spawned entities, Lua globals, `world:declare`d component types.
`RequestOpenProject` tears nothing down itself.

**`RequestOpenProject` checks before it closes.** A directory that is not a game,
or whose `assetsRoot` is unusable, logs an error and returns `false`, and the
open game keeps running. It does not use `ResolveGameProject`, whose "Loading
game" line would claim a load that happens in the next process.

**Unloading a scene does not call `OnStop`.** New and Load destroy the scene's
entities, and the `ScriptRef` OnRemove hook only drops each Lua instance.

**`dt` is clamped, and `mElapsed` accumulates the clamped value.** After a
breakpoint or a window drag it is behind the wall clock on purpose, because it is
simulated time rather than a timestamp and must not be used as one.

The clock is `steady_clock`, so a system clock change cannot produce a negative
`dt`.

**`App` does not own logging or the crash handler.** `main` installs both before
`App` exists, so a construction failure inside `App` is still visible.

**`App` is neither copyable nor movable**, and it hands out references to its own
members through `SystemContext` and `ModuleContext`.

`MakeContext(0.0f)` is used for `Start`, `Stop` and every stage, since none is a
frame and a nonzero `dt` there would be a lie. `mFrame` increments after the
update, so the first tick runs as frame 0.

**`PlayScene`/`StopScene`/`TogglePause` are the only way `EngineModeState`
changes.**

**Only `App.h` and `App.cpp` branch on `MIR_WITH_EDITOR`.** The definition is
PUBLIC because `App.h` lays out `App` differently with it: a file that saw the
other value would disagree with `App.cpp` about the class. The one editor call
left in `App` is the import's failure report in `ApplyImportedAssets`.

**`NewScene`/`SaveScene`/`LoadScene` refuse while Play is running.** These can
corrupt the current scene.

## State

*As of 2026-10-24.* The loop runs, the scheduler ticks, scripts drive entities,
scenes save and load from the editor's File menu, Project > Open Project restarts
into another game, and the scene renders. Play/Pause/Stop work end to end.

No unit tests. `App` is composition, and its behaviour is a window opening, a
device initialising and a loop running. The parts worth asserting — phase
ordering, stages, command buffer flushing, member unwind, asset lookup, scene
round-tripping — are tested where they live. Until a headless renderer exists,
the `smoke.<game>` tests run the whole `App` for 120 frames per game through
`mMaxFrames`, and `smoke.switch` restarts from one game into another twice
through `mSwitchProjectTo`. All of them fail on a crash or a logged error.
`smoke.TerrainCheck` is described in [terrain_runtime.md](terrain_runtime.md).

## Backlog

1. **A headless mode**, which needs the renderer abstraction. It is also the
   prerequisite for any test in this module.
2. **No frame limiter**, and no window-event handling beyond the renderer
   re-querying size.
3. One `App` per process is assumed everywhere, and nothing enforces it.
4. **A second viewport for Play+Paused**, showing the editor camera alongside
   the game view. `RenderSystem::AddViewport` exists (see
   [renderer.md](renderer.md)); nothing calls it yet.
5. **No World diffing for Play/Stop** — `PlaySnapshot` destroys and reloads
   every entity rather than diffing what changed, which is simple but not
   cheap for a large scene.

## Changed

*2026-10-24* — **App builds modules, and transitions run stages.** Was: fixed
subsystem members set up in `Initialize` and torn down in a hand-written
`Shutdown`, every library's components, resources and systems registered in
`Initialize`, every transition's steps written out in `App`, and the editor's
actions applied by `App` mid-frame. Now: a constructor that builds one module
per library as a member, a destructor, stages for the transitions, ImGui drawers
for the UI and a request queue applied at the top of the frame. The script and
mesh resolve passes, the terrain glue, `AssetImportManager`,
`ContentBrowserWatcher`, the scene ops and `CreateGameProject` left `app` for
`script`, `renderer`, `terrain_runtime` and `editor`. Because: a removed
component leaked its runtime object (#222), since only `App` knew what each
library needed, and every new library meant editing `App` in several places.

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
