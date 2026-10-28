# Architecture

*State as of 2026-10-24.* An archetype ECS, a Vulkan 1.3 renderer, an offline
asset pipeline, a Lua scripting layer, an ImGui editor shell, and a thin app
that builds them as modules.

## Principles

**One static library per module.**

Every part of the engine compiles to its own static library under `modules/` for modularity.\
Each module declares its dependencies in its own `CMakeLists.txt`, and the
linker enforces them. A cycle of dependencies will fail the build.

**Expose the bare minimum.**

*Modules*:
Each module has `include/<module>/` for what other consumers may use, and
`src/<module>/` for everything else.

*Third-party libraries*: Libraries should be linked `PRIVATE` by default.If technical exception arises, document and share with team.

**Every library with state or registrations is a module.**

A module is a class, one per library, that `App` holds as a plain member. Its
constructor is its startup: it registers its components, World resources,
systems, component hooks and stage functions, and takes the other modules it
needs as references. Its destructor is its shutdown. `App` declares the members
in dependency order, so C++ builds them in that order and destroys them in
reverse, and a constructor that throws unwinds only what was built.

Every registration returns a move-only `Registration` token, which the module
keeps as one of its last-declared members. The token unregisters when it dies,
so a hook or stage function never outlives the module it calls into.

**Libraries come in three kinds.**

*Base* is `core_base`: logging, paths, file bytes, math, the platform layer and
the crash handler. It is process-wide, set up in `main`, and links only
third-party code. *Plain* libraries hold stateless helpers with no lifetime:
`serial`, `editortheme`, `project` and `physics_layers`. They link base and other plain libraries.
*Modules* are everything else, and may link anything below them. A library
becomes a module only when it owns runtime state or registers something.

**A game is data, not a build target.**

A game lives in `games/<name>/` as configuration, assets, Lua scripts and scene
files. It contains no C++ and adds no target to the build.

**Components are POD, and anything that owns memory is referenced by handle.**

A component must be trivially copyable and standard layout. 

This is because entities are grouped into archetype tables by their exact component set, so adding or removing a component moves that entity's row into a different table.
With POD components, that move is a `memcpy` per column. 

Data that owns memory lives in a world resource, or in the module that manages it, and the component carries a handle to it. More in [core.md](modules/core.md)

**Assets are cooked offline.**

`tools/AssetCooker` converts source files into one container format at build
time. The runtime reads only that format.

An `AssetId` is a hash of the source path relative to the repository root, so the
same file gets the same id on every machine, and any code can compute an id from
a path.

**Setup failures are fatal, and they tell the reader the fix.**

Any module constructor that fails throws a `SetupError` naming the fix. `main`
turns it into an exit code per category, `logs/last-error.json` and an error
box, so a person or an agent can fix the launch without reading engine code. A
crash or `std::terminate` on any thread goes to the crash handler instead, which
writes the same report and a minidump and exits with 20.

Exceptions are allowed, except across a C callback or out of a thread's entry
function. At runtime, an operation that can fail still returns `false`,
`nullptr` or `std::optional`, and logs the reason.

Programmer error uses assertions instead. `MIR_ASSERT` is for development errors and stripped in Release.
`MIR_CHECK` is not stripped an used for runtime-errors for bad inputs. [conventions.md](conventions.md) has the full rule.

## Module graph

The folder tree is in the [README](../README.md#folder-structure). This is what
each library links in the engine, besides `core`; `app` links everything.

| Library | Kind | Links |
|---|---|---|
| `core_base` | base | third-party only |
| `serial` | plain | `core_base` |
| `project` | plain | `core_base`, `serial` |
| `editortheme` | plain | `imgui` only |
| `physics_layers` | plain | `core_base`, `serial` |
| `core` | module | `core_base` |
| `assets` | module | `project` |
| `particles` | module | `assets` |
| `scene` | module | `serial` |
| `prefab` | module | `assets`, `scene`, `serial` |
| `window`, `audio` | module | none |
| `physics` | module | `physics_layers` |
| `input` | module | `window`, `serial` |
| `camera` | module | `input` |
| `renderer` | module | `assets`, `serial` |
| `animation` | module | `assets`, `renderer` |
| `terrain` | module | `assets`, `serial` |
| `particles_runtime` | module, bridge | `particles`, `renderer` |
| `terrain_runtime` | module, bridge | `terrain`, `renderer`, `physics`, `project` |
| `script` | module | `assets` |
| `audio_script`, `input_script`, `physics_script`, `renderer_script`, `terrain_script`, `particles_script` | module, bridge | `script` and its library |
| `imguiruntime` | module | `window`, `renderer`, `input` |
| `gameui` | module | `assets`, `input` |
| `gameui_imgui` | module, bridge | `gameui`, `imguiruntime`, `window` |
| `gameshell` | module | `gameui` |
| `editor` | module | `imguiruntime`, `renderer`, `window`, `input`, `scene`, `animation`, `assets`, `project`, `gameui`, `physics_layers`, `editortheme` |

A cross-library piece moves into a library only when that adds no cycle, no new
third-party code to the library's link closure, and no new runtime setup to its
tests. Otherwise it is a bridge library. `terrain_runtime` is one, so `terrain`
and `TerrainTool` need neither Vulkan nor Jolt. The six `*_script` bridges are
the others, so no library but `script` and its bridges sees Lua.

`gameui` names no graphics API. The `gameui_imgui` bridge puts its draw list in
ImGui's background list and its textures through ImGui, so gameui links neither
`imgui` nor `imguiruntime`. `gameshell` links `gameui`, as a game's own screens
would.

| Module | Owns | Document |
|---|---|---|
| `core` | `core_base`: logging, paths, math, platform, crash handler. `core`: the ECS, stages, frame slots, requests | [core.md](modules/core.md) |
| `project` | `game.json` and `project_settings.json` as plain data | [project.md](modules/project.md) |
| `window` | An OS window and its event pump | [window.md](modules/window.md) |
| `renderer` | The whole Vulkan path, and render-facing components | [renderer.md](modules/renderer.md) |
| `assets` | Cooked blobs, manifest, cache | [assets.md](modules/assets.md) |
| `particles` | Cooked effect definitions, emitter handles and CPU playback lifecycle | [particles.md](modules/particles.md) |
| `animation` | Model pose sampling and per-instance playback data | [animation.md](modules/animation.md) |
| `scene` | Scene save and load, the scene the World holds, the Play snapshot | [scene.md](modules/scene.md) |
| `prefab` | Prefab source/cooked resources, compiled templates and scene instances | [prefab.md](modules/prefab.md) |
| `serial` | JSON documents on disk | [serial.md](modules/serial.md) |
| `script` | Lua embedding, ECS bindings, the script resolver | [script.md](modules/script.md) |
| `physics` | Jolt world, `RigidBody`, the step, layers, events | [physics.md](modules/physics.md) |
| `*_script` | Six bridges adding one library's Lua bindings each | [script-bridges.md](modules/script-bridges.md) |
| `terrain_runtime` | Terrain streaming, drawing and collision at runtime | [terrain_runtime.md](modules/terrain_runtime.md) |
| `audio` | Wwise lifetime and the per-frame pump | [audio.md](modules/audio.md) |
| `camera` | Gameplay camera control: follow a target, or free-fly from input | [camera.md](modules/camera.md) |
| `imguiruntime` | ImGui context lifetime, fonts, both backends, the frame bracket | [imguiruntime.md](modules/imguiruntime.md) |
| `editor` | The editor shell, drawn into `imguiruntime`'s context | [editor.md](modules/editor.md) |
| `editortheme` | Theme data only | [editortheme.md](modules/editortheme.md) |
| `gameui` | Game UI drawn over the scene, the same in the editor and a game build | [gameui.md](modules/gameui.md) |
| `gameshell` | The HUD and the pause menu every game gets, built on `gameui` | [gameshell.md](modules/gameshell.md) |
| `gameui_imgui` | The game UI's ImGui backend and drawer | [gameui_imgui.md](modules/gameui_imgui.md) |
| `app` | The module members, scene commands, the frame loop | [app.md](modules/app.md) |

Add your row when you start a document, and update it in the same commit as the
code it describes.

## Cross-module rules

**`core` depends on nothing in the engine.**

Everything else may depend on it.

**`app` is the only module that knows all the others.**

It holds each module as a member, declared in dependency order: a module after
every module it uses, and a bridge after `script` and its library. Within a
system phase, systems run in the same order. Adding a module means adding one
member, which forces a decision about where it belongs.

**A same-phase order follows a dependency edge.**

Within a phase, a system may rely on running after another only when its
library depends on the other's. Any other ordering needs a phase boundary, which
is why `Propagate` exists: `PhysicsInterpolationSystem` writes `Transform` in
`PostUpdate`, `TransformPropagateSystem` runs alone in `Propagate`, and every
`WorldTransform` reader runs in `LateUpdate`.

**`renderer` never sees `window`.**

It takes an `ISurfaceProvider`, declared in `core/platform/Surface.h`, which
exposes `NativeWindow()`, `Width()` and `Height()`.

`engine_renderer` does not link `engine_window`, and no GLFW type appears in a
renderer translation unit.

Anything that can produce a native handle can therefore drive the renderer.
Anything else the renderer comes to need — DPI scale, HDR — is added to
`ISurfaceProvider` deliberately.

**Render-facing components live in `renderer`.**

`MeshRenderer` and `Camera` hold handles that the renderer allocates, validates
and destroys, so they are declared in `renderer/include/renderer/components/`.

`engine_core` never depends on `engine_renderer`. A consumer of `core` that does
not render never pulls Vulkan in through a component header.

A module adding its own render-facing component follows the same rule.

**`imguiruntime` owns the ImGui context's lifetime, and nothing else does.**

It creates the context, loads the fonts, initialises the GLFW backend and,
through `VulkanRenderer::InitImGuiVulkanBackend`, the Vulkan one, then tears all
of it down in reverse. It also brackets the frame: `BeginFrame` is the
`NewFrame` trio and `EndFrame` is `Render` plus `GetDrawData`. Every UI module,
`editor` today and `gameui` next, draws between those two calls and owns no part
of the lifetime.

It is its own module because neither obvious owner may hold it.
`ImGui_ImplGlfw_InitForVulkan` needs a `GLFWwindow *`, which `renderer` may not
see, and `App.cpp` may not call ImGui.

`editor` keeps what only an editor wants: the theme and its DPI scale, docking,
and `imgui.ini`. The runtime leaves `IniFilename` null, so a build without
`editor` persists nothing.

`ImGuiModule::RunUI` brackets the frame and runs the drawers modules registered,
the `Tools` group before the `Game` group: the editor's layout first, then the
game UI, which fits the scene viewport the editor wrote that frame. It hands
the draw data to the renderer and the frame's keyboard and mouse capture to
`InputState`. `App.cpp` contains no ImGui call and includes no ImGui header.

**The game UI draws into ImGui's background draw list and nowhere else.**

That list renders before every window, and the renderer draws ImGui after the
3D views, so the game UI lands over the scene and under the editor's panels. The
same code is a HUD clipped to the scene viewport in the editor and fullscreen UI
in a game build, with no renderer change. Only `gameui_imgui`'s
`ImGuiUIBackend.cpp` includes ImGui.

**`editortheme` depends on nothing engine-side, only `imgui`.**

It is theme data, applied once by `editor` at startup.

**File bytes move in `core`, and JSON in `serial`.**

`core/fs/FileIO.h` opens every file the engine reads or writes, and
`std::ifstream` and `std::ofstream` appear in no other translation unit.

`serial/JsonFile.h` is the only place that decides those bytes are JSON. It is
its own module rather than part of `core` because it returns `nlohmann::json`,
which would otherwise make that library a public dependency of the module
everything else depends on.

A module above those two decides what a document means, and names no stream
type. The flow is in [A file](#a-file).

## Flows

### A frame

`App::Run` is a variable-dt loop; only the `FixedUpdate` phase steps at a fixed rate.

```
PollEvents                       close flag and framebuffer size refresh here
dt = min(elapsed, mMaxDeltaSeconds)
ApplyRequests                    last frame's Play, Stop, Load, Save, Quit, import swap, project switch
RunFrameSlot(FrameBegin)         script reload poll, terrain installs and Edit refresh, import and content browser polls
ImGuiModule::RunUI               editor drawer, then the game UI drawer; draw data and UI capture handed on
FixedTimestep::Advance(dt)       substeps this frame, mFixedDeltaSeconds each, at most mMaxFixedSteps
SystemScheduler::Update          PreUpdate, Update, FixedUpdate x substeps, PostUpdate, Propagate, LateUpdate, Render
```

A transition someone asks for mid-frame waits in the `RequestQueue` World
resource and runs at the top of the next frame. Nothing is drawn or running
there, so a transition never frees what this frame's draw data or systems still
hold.

`ScriptSystem` runs in `PreUpdate`, after `ScriptResolveSystem` gives every
`ScriptRef` its instance. `FixedUpdate` is for physics and its sync back to
`Transform`; it runs zero or more times a frame with the fixed step as `dt`.
Before each step `PhysicsSystem` calls the `PhysicsStepGates` World resource's
gates; terrain is one, placing collision pieces and holding bodies whose ground
is not collidable yet (#128). `TransformPropagateSystem` runs in `Propagate`,
after every `Transform` writer, and `RenderSystem` runs in `Render`, after all
of them, so every `WorldTransform` it reads is current for the frame being
drawn.

`PrefabSystem` also runs first in `PreUpdate`, but publishes compiled Prefab
changes only in Edit; Play freezes the registry and every instance. A Prefab
source rescan during Play waits for the next Edit commit.

The scheduler flushes the command buffer at each phase boundary and after each
`FixedUpdate` substep, never in the middle of a system.

`RenderSystem` holds a `VulkanRenderer&` passed to its constructor. The renderer
is not published as a world resource, which keeps it out of reach of scripts.
Draws that depend on the viewport come from the `ViewDrawSources` resource:
`RenderSystem` hands each source every viewport's camera before it builds any
view, and app's terrain answers per view with its own level of detail (#125).
Meshes queued with `QueueMesh` are copied at the start of the next recorded
frame within a per-frame byte budget and become drawable once that frame has
completed; retired ones are freed after their last frame, with no wait (#127).

### A transition

`App` runs a scene or Play transition as a fixed sequence of its own steps and
stages. A stage runs the functions modules registered for it, in registration
order, then flushes the command buffer.

```
New / Load        SceneUnload  ->  scene replaced  ->  SceneLoad
Save              written      ->  SceneSaved
Play              PlayBegin    ->  snapshot  ->  Play mode
Stop              PlayEnd  ->  restore the snapshot  ->  Edit mode  ->  SceneLoad
Asset reload      BeginReload  ->  ReloadPrepare  ->  CommitReload  ->  ReloadCommit
                                     (a veto)  ->  ReloadAbort  ->  AbortReload
```

A `SceneLoad` function reads why the scene is there through
`SceneModule::LastLoad()`: from disk, new, or restored from the Play snapshot,
and whether it is incomplete. A `ReloadPrepare` function vetoes the swap by
throwing, and `ReloadAbort` then drops what every holder staged. Stop keeps the GPU caches, which are keyed by asset; per-entity state
goes in the owning component's `OnRemove`, which the restore fires for every
Play entity.

### A file

Naming a file and moving its bytes are separate mechanisms. A path is resolved
once at startup into a `GameProject`, and bytes always travel the same three
layers.

A game directory is named by `--game` or by `defaultGame` in `engine.json`, and
every path is built from `ExecutableDir()`.

```
--game <dir>      engine.json "defaultGame"
         \             /
     NormalizeGameDir()          rejects anything outside ExecutableDir()
             |
     LoadGameProject()           reads game.json: assetsRoot, sceneDir, soundBanksRoot
             |
        GameProject
             |
             +-- mProjectSettingsPath --> InputMap, RenderSettings
             +-- mSceneDir            --> SceneManifestPath(), EntityFilePath()
             +-- mSoundBankDir        --> audio::Init, after which Wwise opens banks itself
             +-- mAssetIdRoot         --> an id prefix, not a disk path
```

`GameProject` holds the directories while each module holds its own filenames, so
a module joins a root to its own constant. `kSceneManifestFileName` lives in
`scene`, `kAssetManifestFileName` in `assets`, and the three project-level names
in `project/GameProject.h`. A module that spells another module's filename
prevents that module from renaming its own file. The asset id root is the one
field `project` cannot fill: `assets`' `ResolveAssetIdRoot` does, with the
normalizer the cooker uses.

A key missing from `game.json` falls back to a default in `GameProject.cpp`. The
`assetsRoot` default is written a second time in the root CMakeLists, which reads
the same key at configure time to decide what to cook, and the two have to agree
or the cooker and the runtime hash ids from different trees.

Cooked assets are named the other way round, by id rather than by path.

```
scene field  "meshes/cube.mesh.json"
             |
     mAssetIdRoot + "/" + name
             |
     MakeAssetId()               FNV-1a 64 of the path string
             |
     AssetManifest::Find(id)
             |
     CookedAssetsDir() / PathOf(entry)
```

A scene names an asset by a path that is never opened. The string is hashed into
an `AssetId`, the manifest turns that id into a cooked file, and a real path
exists only at that last step ([An asset](#an-asset)). That hash is also why
`assetsRoot` cannot change once a build has cooked.

Bytes then move through one stack, whichever lane named the file.

```
GameProject  InputMap  RenderSettings  SceneIO    ScriptLoader   AssetCache   VulkanRenderer
     |          |            |            |            |              |             |
 ReadJsonFile / WriteJsonFile / UpdateJsonFile      ParseJson         |             |
     |          |            |            |         (no path)        |             |
     +----------+------------+------------+                          |             |
                |                                                    |             |
      ReadText / WriteTextAtomic                                 ReadBytes     ReadBytes
                |                                                    |             |
                +----------------------------------------------------+-------------+
                                        |
                             std::ifstream / std::ofstream
```

`ReadJsonFile` is `ReadText` followed by `ParseJson`, and `WriteJsonFile` is
`dump()` followed by `WriteTextAtomic`, so the `try`/`catch` around a parse
appears once in the engine. `AssetCache` and `ReadSpirv` skip `serial` because
they want bytes rather than a document. The script loader holds bytes already,
out of an `AssetBlobView`, and hands them to Lua without a parse.

Writes through `serial` are atomic, because JSON on disk here is always a file a
person can edit. `AssetCooker` writes with plain `WriteBytes`, since a tool
regenerates its output.

A missing file is quiet and a malformed one is logged, so a caller that needs
them apart tests for the file first. `LoadGameProject` does, because a missing
`game.json` means the directory is not a game while a malformed one means a game
that falls back to defaults.

### An asset

A source file under a cooked root is read by `tools/AssetCooker` at build time.
The cooker writes one blob per file, named by its `AssetId`, plus a manifest. The
build copies both next to the executable, and `AssetCache::Load(id)` reads them
at runtime.

The cooked roots are the engine's `assets/` plus each game's `assetsRoot`. The
root CMakeLists reads those from every `games/*/game.json` at configure time, so
adding a game adds its assets without touching CMake.

A `*.prefab` file is cooked to a typed payload under the id
`prefab:<uuid>`, not the path hash every other asset uses. An editor build scans
the source `prefabs/` folder and reads the files directly; a build without the
editor removes that copied source folder next to the executable and fills the
Prefab registry from the cooked manifest instead. Both reach the same lookup by
`PrefabId`, so a scene's instance record never names a source path.

An `AssetId` is the FNV-1a 64 hash of the source path relative to the repository
root. That is why `engine_cook_assets` rejects an absolute cook root: the root
string is part of what gets hashed, so an absolute path would bake one machine's
checkout location into ids that have to match on every machine.

Shaders do not go through this. They are compiled by `slangc` into `.spv` files
and copied next to the executable separately.

### A scene

`scene/SceneIO.h` writes a scene as a directory. `scene.json` lists `StableId`s
in load order, `entities/<id>.json` holds one ordinary entity each, and
`instances/<root>.json` holds a Prefab instance: its Prefab UUID, placement,
one binding per compiled node address, and the scene's overrides.

Splitting by entity keeps a diff proportional to what changed, and lets two
people edit different entities in the same scene without conflicting. A Prefab
instance is one record because its members come from a template; saving every
member separately would make the template and the instance fight over the same
component values.

`LoadScene` materializes instances through `ScenePrefabResolver`, which
compiles the template in `prefab` and builds one entity per bound address. The
resolver reports each template `EntityRef` default, so a save can write a patch
only where the scene differs.

Components round-trip generically, through the `FieldDesc` tables registered in
`ComponentRegistry`. Giving a component a field table is therefore the entire
serialization cost of that component, and it works the same way for a
script-declared component as for a C++ one.

Scenes are not a cooked asset - `SceneIO` reads/writes them directly under
`games/<name>/scenes/<sceneName>/`, next to the executable (`GameProject`
resolves every path there against `ExecutableDir()`, not the repo), because
`SaveScene` has to write them back at runtime. The root CMakeLists seeds
that directory (and `project_settings.json`) from the repo's checked-in
copy once, at first configure, and never touches it again: unlike
`assets/`/`game.json`, which have no runtime writer and safely re-mirror
from source on every build, doing that to `scenes/` would silently discard
whatever the running editor last saved the moment the project is next
rebuilt.

The three load passes are in [scene.md](modules/scene.md).

### A Prefab

A `.prefab` source becomes a `PrefabDocument`, then a `CompiledPrefab`. The
compile flattens every nested occurrence into one node list, with addresses that
prepend the occurrence ids, and turns every `EntityRef` into a relocation to
another compiled node. `CompiledPrefabCache` keeps that template until a newer
document compiles.

A scene instance is the bridge back to `StableId`s. Its record binds each
compiled address to one, and its patches hold only what the scene overrides.
`PrefabCoordinator` binds that scene, and its `PrefabSystem` publishes compiled
edits and nested dependency changes at the next Edit frame boundary. During
Play it is frozen, so no template change can restructure entities the Play
snapshot owns.

An open Prefab document gets its own preview World and `LoadedScene`, so the
editor can edit it without touching the scene. `PrefabAuthoring` records the
authored entries for both, and `EditorModule` saves a Prefab's unsaved dependencies
before the scene that uses them.

### A script

A `.lua` file is a cooked asset. `ScriptHost` loads its source, `CreateInstance`
returns an `instanceRef`, and a `ScriptRef` component links an entity to that
instance.

`ScriptResolveSystem` gives every `ScriptRef` without an instance one, and
replaces the instance of a renamed one, every frame and in every mode. Whatever
made the `ScriptRef` (a scene load, Stop, a duplicate, an undo, a prefab
publish) needs no resolve call of its own. `ScriptSystem` then walks
`Query<ScriptRef>` in `PreUpdate` and calls into Lua per entity.

Scripts reach components by name, through `ComponentRegistry`. Exposing a new
native component to Lua therefore costs no new binding code — it costs a field
table. See [script.md](modules/script.md).

## Known gaps

1. No input. `window` surfaces no events at all.
2. No renderer abstraction. `Renderer.h` is a placeholder and `App` names
   `VulkanRenderer` directly, so there is no headless mode, and nothing in
   `app`, `renderer` or `editor` is unit tested, except `ContentBrowserWatcher`
   (see [modules/editor.md](modules/editor.md)), whose disk-polling logic has no
   renderer/ECS dependency to need one.
3. No typed assets. Everything cooks as a raw passthrough blob.
4. Linux sources exist, but there is no Linux preset. macOS fails at configure.
5. No CI. Tests are run by hand.
6. **The Content Browser's rename detection is best-effort.** A rename is
   recognized only when it is the single remove+add pair in a 250ms poll tick
   with a matching size+mtime signature; renaming/moving an asset never
   rewrites scene references to its old `AssetId` (see
   [modules/assets.md](modules/assets.md)).

## Changed

*2026-10-24* — **App builds modules instead of wiring subsystems.** Was: `App`
held fixed subsystem members, registered every library's components, resources
and systems by hand in `Initialize`, and ran every scene transition's steps
itself, with a hand-written `Shutdown` order. Now: one module class per library,
held as members in dependency order; each registers its own parts and frees
them in its destructor, transitions run stages that modules register into, and
UI runs as ImGui drawers. `core` split into `core_base` and `core`, and
`project`, `terrain_runtime` and five script bridges joined the graph. Because:
removing a component leaked its runtime object (#222), since nothing but `App`
knew what each library needed, and every new library meant editing `App` in
several places. See [app.md](modules/app.md) and [core.md](modules/core.md).

*2026-10-20* — **Setup failures are fatal and reported.** Was: failures returned
a value, a failed startup exited with `-1` and no message, and several services
continued degraded (no audio, no splash, no asset import). Now: every setup
failure throws a `SetupError`, and `main` reports it through an exit code, a
report file and a box. Because: a degraded start hid broken data, and a silent
exit gave the reader nothing to act on. See [conventions.md](conventions.md#errors).

*2026-10-02* — **The editor is optional.** Was: `app` linked `editor`
unconditionally. Now: `MJOLNIR_WITH_EDITOR` OFF, set by the
`windows-msvc-game-*` presets, builds `MjolnirEngine` as the game with no
editor, `editortheme` or ImGuizmo in the build, which is what moving ImGui's
lifetime into `imguiruntime` made possible. See [build.md](build.md#a-build-without-the-editor).

*2026-10-02* — **`gameshell` added.** Was: the only game UI was
`gameui`'s own demo screen. Now: a HUD and a pause menu, in a module that
links `gameui` alone, drawn by `App` in Play, and a game build started with
`--no-editor` begins in Play. See [gameshell.md](modules/gameshell.md).

*2026-10-02* — **`gameui` links `assets` instead of `renderer`.** Was: it
linked `renderer` for `TextureHandle`, which had no way into ImGui. Now: it
loads cooked textures from the `AssetCache` and hands them to ImGui 1.92's
dynamic texture path, which the Vulkan backend uploads and frees. See
[gameui.md](modules/gameui.md).

*2026-09-27* — **`gameui` added.** Was: `editor` was the only thing drawing into
the ImGui frame, so a game had no UI. Now: `gameui` records a draw list and
replays it into ImGui's background draw list, over the scene and under the
editor's panels, clipped to the scene viewport in the editor and fullscreen
without it. See [gameui.md](modules/gameui.md).

*2026-09-26* — **ImGui's lifetime moved out of `editor` into `imguiruntime`.**
Was: `Editor::Initialize` created the context, loaded the fonts and initialised
both backends, and `Editor::BeginFrame`/`EndFrame` bracketed the frame, so any
other UI module would draw into a context the editor owned and there could be no
editor-free build. Now: `imguiruntime` owns all of that, and `editor` draws
between its `BeginFrame` and `EndFrame` as `gameui` will. See
[imguiruntime.md](modules/imguiruntime.md).

*2026-09-19* — **Descriptor sets and a per-frame camera UBO replaced the
premultiplied view-projection push constant.** Was: `RenderSystem` baked
`viewProj * world.Matrix()` into `DrawItem::model` on the CPU, so the camera
travelled as part of the 128-byte push constant. Now: `DrawItem::model` is
just the world transform, `RenderView` carries its own `viewProj`, and
`VulkanRenderer` writes it into a per-frame, per-view slice of a dynamic
uniform buffer (set 1) that the vertex shader combines with the model push
constant. See [renderer.md](modules/renderer.md).

*2026-09-18* — **File bytes moved to `core`, and JSON to `serial`.** Was:
`assets/AssetFileIo.h` held the byte primitives, so `input` and `renderer` linked
`mir::assets` to read a config file that is not an asset, and `app`, `scene` and
the SPIR-V loader opened their own streams besides, each with its own
`json::parse` try/catch and its own log wording. Now: `core/fs/FileIO.h` and
`serial/JsonFile.h`, with the flow in [A file](#a-file) and the per-module detail
in [core.md](modules/core.md#changed) and [serial.md](modules/serial.md).

*2026-09-14* — **The runtime loads a game directory.** Was: a `HelloWorld`
executable whose `main.cpp` hard-coded `games/HelloWorld` paths and built a demo
scene when none was saved. Now: `MjolnirEngine --game <dir>`, defaulting to
`defaultGame` in `engine.json`. Paths in `game.json` are relative to the game, a
directory without `game.json` is refused, and the root CMakeLists cooks every
game's assets and registers a `smoke.<game>` test for it.

*2026-09-03* — **`RenderSystem` replaced hand-driven drawing.** Was: `App::Run`
called `CollectDrawInstances` and `DrawFrame` by hand between scheduler phases,
so draw-list assembly had no defined ordering against the systems that produce
what it draws. Now: a normal system in `SystemPhase::Render`. Ordering comes from
phase placement, and `App::Run` no longer knows a `DrawItem` exists.

*2026-09-03* — **ImGui moved out of `App` into `editor`.** Was: about 150 lines
of `ImGui::*` and `ImGui_Impl*` calls in `App.cpp`, with `App.h` needing
dockspace internals just to declare a member. Now: a module with a six-call
surface.

*2026-09-03* — **The scene scissor rect comes from the dockspace central node.**
Was: read from `ImGui::Begin("Scene")`'s content region. That compiled and ran
with no warning, and clipped the scene to a 300x650 sidebar, so the cube rendered
into nothing. Now: read only from `DockBuilderGetCentralNode` and exposed as
`Editor::SceneViewportRect()`, and no window named `"Scene"` exists. Anyone
adding a real named viewport window has to update that source, or the same bug
comes back silently.
