# script

Embeds Lua as the gameplay scripting layer, through sol2 over LuaJIT. A script
reads per-frame ECS state and drives triggers, entity behaviour and UI logic. It
reaches physics only through the `physics_script` bridge, and never draws a frame itself - its
one reach into rendering is the post-processing settings resource, a handful of
plain numbers, not the renderer's own state.

`mir::script` · depends `mir::core` (public); `mir::assets`, `mir::audio`, `mir::input`, `mir::physics`, `mir::renderer`, `mir::terrain`, `sol2`, `luajit` (private) · API `modules/script/include/script/` · maintainer Rahul Nair · reviewed 2026-09-18

## Model

Exposing a new native component to Lua costs no new binding code. This module is
the main consumer of `core`'s `ComponentRegistry` and `FieldDesc` name erasure,
which was built for exactly this. Scripts reach components by name, so the
bindings never mention a C++ component type.

There are two lifetimes, kept separate.

**Code** is one Lua table per loaded script *name*, holding only the functions
that script defines — `OnStart`, `OnUpdate`, `OnFixedUpdate`, `OnStop` and `NewInstanceData`. It
is shared across every entity running that script.

**State** is one small private table per `instanceRef`, passed as `self` to every
callback.

`ScriptRef` is a POD component carrying `instanceRef`, `started` and
`scriptName`, and it is the only thing the ECS knows about a script.

`ScriptResolveSystem` gives every `ScriptRef` its instance. It runs first in
`PreUpdate`, every frame and in every mode, and creates an instance for every
`ScriptRef` that has none, loading the script's cooked source the first time
the name appears. It remembers the name each instance came from, so a rename
replaces the instance, and it does not retry a name that failed to load until an
asset reload. Whatever made the `ScriptRef` needs no resolve call of its own.

`ScriptSystem` is the sole driver. It is one `ISystem` in
`SystemPhase::PreUpdate`, after the resolver, walking `Query<ScriptRef>`,
calling `OnStart` once and then `OnUpdate` every frame. Each entity's callback is isolated from the others
by a C++ `try`/`catch` and by sol2's `protected_function`, so a script error
stops that entity's script rather than the frame.

`ScriptFixedSystem` calls `OnFixedUpdate(self, world, entity, dt)` once per physics step, with
the fixed step as `dt`. It runs in `SystemPhase::FixedUpdate` at `SystemOrder::kEarly`, before
`PhysicsSystem`, so forces and velocities a script sets there act on the same step. A frame with no
step never calls it, and a frame that catches up several steps calls it once for each. It skips a
script whose `OnStart` has not run yet, so `OnStart` always comes first, and it isolates each
entity's callback like the others.

`ScriptModule` owns the host, the reload watcher and the three systems. It sets the
`ScriptRef` OnRemove hook, which drops the instance, polls the watcher in the
`FrameBegin` slot, stops every script at `PlayEnd`, and lets the resolver retry
failed names at `ReloadCommit`. Every library's own Lua bindings come from a
bridge (see [script-bridges.md](script-bridges.md)); `script` itself binds only
`Entity` and `World`.

```lua
local Spin = {}
local speed = 1.0

function Spin.NewInstanceData()
    return { angle = 0.0 }              -- per-instance; never shared
end

function Spin.OnUpdate(self, world, entity, dt)
    self.angle = self.angle + speed * dt
    local half = self.angle * 0.5
    world:set(entity, "Transform", "rotation", { w = math.cos(half), x = 0.0, y = math.sin(half), z = 0.0 })
end

return Spin
```

## API

| Type | Header | Role |
|---|---|---|
| `IScriptHost` | `script/IScriptHost.h` | The abstract seam — the only thing outside `script/src` should depend on |
| `ScriptHost` | `script/ScriptHost.h` | The one Lua-backed implementation; sol2 hidden behind a pimpl |
| `ScriptRef` | `script/components/ScriptRef.h` | POD link component: `instanceRef`, `started`, `scriptName` |
| `ScriptSystem` | `script/ScriptSystem.h` | Drives the callbacks per `ScriptRef`, `SystemPhase::PreUpdate` |
| `ScriptResolveSystem`, `MakeScriptAssetLoader`, `ReleaseScriptInstance` | `script/ScriptResolveSystem.h` | Gives every `ScriptRef` its instance; loads a script from the cooked assets; drops an instance |
| `ScriptModule` | `script/ScriptModule.h` | The host, the watcher, both systems, the hook and the stage functions |
| `AddBindings`, `LuaBindings` | `script/BindingApi.h` | How a bridge adds its bindings; behind the `script_bindings` target, the only header with sol2 in it |
| `CallScriptCallback`, `LuaState` | `script/BindingApi.h` | Calls a callback a bridge defines, such as `gameui_script`'s `OnUI`, isolated and counted like `OnUpdate`; the host's Lua state, for the values a bridge passes it |
| `ScriptReloadWatcher` | `script/ScriptReloadWatcher.h` | Polls a tracked script's cooked file and reloads on change |
| `RegisterScriptComponents` | `script/ComponentRegistration.h` | Registers `ScriptRef`, mirroring `core` and `renderer` |

The Lua surface — `world:has/get/set/spawn/destroy/add/remove/each/declare` and
the `Entity` usertype — lives in `src/script/bindings/` and is not public. Loaded
Lua source reaches it; no C++ outside this module includes it. The input,
physics, audio, terrain and post-processing bindings live in the bridges, and
each extends that same `World` usertype rather than adding one of its own. The
host binds `Entity` and `World` when it builds its state, so every bridge's
bindings run after them.

`world:pause()`, `world:resume()`, `world:restart()` and `world:quit()` push
the pause menu's requests, for a game that draws its own: `restart` is Stop then
Play, and `quit` stops Play in the editor and closes a game build. Each returns
`true` when queued, and outside Play does nothing and returns `false`.
`world:is_paused()` says whether Play is paused.

`world:load_scene("sceneId")` queues a swap of the running Play scene to a
scene registered in `game.json`. It returns `true` when the request is queued.
Scene files prepare on a worker while the current scene continues running.
The swap runs at a frame boundary after preparation finishes. An unknown ID or a scene that
cannot be prepared leaves the current scene running and logs an error.
The latest queued scene replaces a pending request, and Stop cancels it.
`SceneEnd` stops only the outgoing scene's scripts. `world:persist(entity)`
keeps an entity and its children across the swap, `world:scene_instance()`
returns the active instance ID, and `world:session_data()` returns a table that
survives swaps for the length of Play.

Additive loads keep the other loaded scenes simulating and rendering together:

```lua
local interior = world:load_scene_additive("Interior")
world:set_primary_scene(interior) -- applies when this instance finishes loading

-- Later, once world:scene_loaded(interior) is true:
local doorway = world:find_scene_entity(interior, 1)
if doorway and world:is_alive(doorway) then
    -- Use this instance's saved entity 1.
end

world:unload_scene(interior) -- also cancels an instance still preparing
```

`load_scene_additive` returns a unique instance handle immediately, or `nil`
outside Play. Loading the same scene twice produces two handles. `unload_scene`
removes only that instance and its spawns. Unloading the primary promotes the
oldest remaining instance. With no loaded scenes, `world:primary_scene()` is 0
and ordinary spawns belong to the session. `load_scene` replaces every loaded
scene and cancels pending additive loads, preserving session data and persistent
entities.

`world:spawn()` inside a scene script belongs to the script's scene.
`world:spawn(instance)` chooses a loaded scene explicitly, or session ownership
with 0. It returns `nil` for an unavailable instance. `world:entity_scene(entity)`
returns its owner, or `nil` for a dead entity. A persistent script's default spawn
owner is the primary scene. Primary selection also prefers that scene's flagged
camera, while each scene keeps its own active-camera flags.

`find_scene_entity(instance, stableId)` resolves a saved entity in a particular
loaded instance. Runtime Entity handles may cross scenes, and become invalid when
their owning scene unloads. Use `is_alive` before using a retained handle. Scene
files save references within their own scene, cross-scene links are resolved at
runtime. Persisted entities have session ownership and no longer resolve through
their former scene's saved ID.

The input half is `world:is_down/just_pressed/just_released/axis/axis2d` and
`world:active_device`. Scripts are gameplay, so the action reads go through
`InputState::ForGame`: every action reads at rest while a game UI screen, such
as the pause menu, holds the input. See [gameui.md](gameui.md).

The audio half is `world:post_event/stop_event/is_playing/set_rtpc/set_switch/set_state`.
All but `set_state` take the entity and reach the `AkGameObject` its
`AudioEmitter` carries; `set_state` takes none, because a Wwise state is global.
`is_playing` is true while the voice the emitter last started, by `post_event`
or `playOnStart`, is still playing.

```lua
function T.OnUpdate(self, world, entity, dt)
    self.charge = math.min(self.charge + dt, 1.0)
    world:set_rtpc(entity, "Speed", self.charge * 100.0)   -- false if refused
end
```

The music and SFX busses have four each, which take no entity:
`world:set_music_volume(volume)`, `world:music_volume()`,
`world:set_music_muted(muted)` and `world:music_muted()`, and the same with
`sfx` for `music`. The volume is linear, from 0 to 1; a value outside that, NaN,
infinity, or a mute that is not a boolean is refused with an error and the old
value stays, so it never reaches Wwise. They are kept while the sound engine is
down and applied when it starts, so a settings screen can write them at any time,
and the engine saves them per user. Muting keeps the volume to come back to. With
both channels silent, `world:post_event` returns 0: see
[audio.md](audio.md).

```lua
if world:just_pressed("MusicMute") then
    world:set_music_muted(not world:music_muted())
end
```

The renderer bridge adds `world:mesh_bounds(entity)`: the local-space
`{ min = {x, y, z}, max = {x, y, z} }` box of the entity's resolved mesh, or
`nil` when there is none yet. `MeshBounds` is derived and never registered, so
`world:get` cannot reach it.

`world:get_active_camera()` returns the entity whose `Camera` is active, or
`nil`. `world:set_active_camera(entity)` flags that camera and clears every
other one at once, so the change shows the same frame; it returns `false` and
logs an error if the entity has no `Camera`. `world:cut_camera(entity)` tells
the renderer that camera jumped (call it after teleporting the camera or its
rig), so its next frame blends nothing from the frames before; it returns
`false` and logs an error the same way. Switching cameras needs no cut.

### Post-processing

`world:set_fog/get_fog`, `world:set_hdr/get_hdr`, `world:set_bloom/get_bloom`,
`world:set_depth_of_field/get_depth_of_field`, `world:set_film/get_film` and
`world:set_motion_blur/get_motion_blur` reach `PostProcessingSettings`
- one resource shared by the whole render, so (like `set_state`) these take no
entity. Exposure, focus distance, aperture and shutter strength are `Camera`
fields instead, set with `world:set` on the camera; `set_hdr`,
`set_depth_of_field` and `set_motion_blur` refuse those keys with an error
that says so. Each `set_*` takes one table; every
field is optional and defaults to that effect's *current* live value, so a
script can tweak one knob without restating the rest. A present-but-wrong-shaped
field (a string where a number belongs, an unknown `tonemapOperator`) refuses
the whole call - nothing is written - the same strictness `world:set` already
uses for component fields. Each `get_*` returns a table of every field, or
`nil` if this world has no `PostProcessingSettings` at all.

```lua
world:set_bloom({ enabled = true, threshold = 1.0, intensity = 0.5, radius = 1.0 })
world:set_fog({ color = { x = 0.5, y = 0.6, z = 0.7 }, density = 0.015, startDistance = 0, endDistance = 300 })
world:set_hdr({ tonemapOperator = "aces" })  -- "none" | "reinhard" | "aces"
world:set_depth_of_field({ enabled = true, maxBlur = 16.0 })
world:set_film({ enabled = true, grain = 0.05, contrast = 1.0, saturation = 1.0,
                 vignetteStrength = 0.3, vignetteRadius = 1.0 })
world:set_motion_blur({ enabled = true, sampleCount = 8 })

local camera = world:get_active_camera()
world:set(camera, "Camera", "exposure", 1.5)
world:set(camera, "Camera", "focusDistance", 10.0)

local bloom = world:get_bloom()
if bloom.enabled then
    world:set_bloom({ intensity = bloom.intensity * 0.9 })  -- everything else untouched
end
```

`ReadFogParams`/`WriteFogParams`/`ReadHdrParams`/`WriteHdrParams`/
`ReadBloomParams`/`WriteBloomParams`/`ReadDepthOfFieldParams`/`WriteDepthOfFieldParams`/
`ReadFilmParams`/`WriteFilmParams`/`ReadMotionBlurParams`/`WriteMotionBlurParams`
(`renderer/FogEffect.h`, `renderer/HdrEffect.h`, `renderer/BloomEffect.h`,
`renderer/DepthOfFieldEffect.h`, `renderer/FilmEffect.h`, `renderer/MotionBlurEffect.h`) are the same functions
the Post-Processing panel itself reads and writes - this binding can't drift
from what the panel shows, in either direction.

### Terrain height query (#144)

`world:terrain_height_at({x=..., y=..., z=...}, requiredSpacing)` queries the
resident `TerrainWorld` at a world position. `requiredSpacing` is the largest
acceptable spacing in meters. All three coordinates and spacing must be Lua
numbers; coordinates must be finite and fit `WorldReal`, and spacing must be
finite and positive. Missing fields, wrong types, missing or extra arguments
log an error and return `Failed` / `InvalidInput`, even without terrain.
Double builds preserve Lua doubles through `WorldPosition::FromDoubleMeters`.

```lua
local ground = world:terrain_height_at({x=150000.500123, y=10, z=-150000.250321}, 1)
if ground.hasHeight then
    print(ground.worldHeight, ground.spacing, ground.maxError, ground.releaseId)
end
```

| Result field | Meaning |
|---|---|
| `status` | `MeetsRequirement`, `CoarserWhileLoading`, `CoarserAtSourceLimit`, `Loading`, `OutsideMap`, or `Failed` |
| `failure` | `None`, `WrongRelease`, `InvalidInput`, `LoadFailed`, or `InvalidSamples` |
| `hasHeight` | True for the three height-bearing statuses |
| `localHeight` | Final terrain-local height; nil without a height |
| `worldPoint`, `worldHeight` | `{x,y,z}` surface point and its world y; nil without a point |
| `spacing`, `sourceSpacing` | Answering and finest baked spacing; nil when C++ has no finite value |
| `maxError` | C++ bound against the finest baked surface; nil when unmeasured |
| `releaseId` | Lowercase 16-digit hexadecimal release id |
| `release` | Full identity: hexadecimal `assetId`, `sourceFingerprint`, `releaseId`, numeric `formatVersion` |
| `tile`, `layer` | `{x,z}` signed tile and zero-based layer, exactly as returned by C++ |

Unknown identities retain the C++ zero sentinel as `0000000000000000`.
Missing heights never become zero; a real zero remains a number. Tile and
layer fields retain C++ defaults on failures and are meaningful with the
corresponding status. There is no sample/cell field in `TerrainHeightResult`.

### Terrain layer weight query

`world:terrain_layer_weights_at({x=..., y=..., z=...})` queries the resident
`TerrainWorld` for the material layer weights of the ground at a world
position. Its arguments follow `terrain_height_at`: all three coordinates must
be Lua numbers, finite and fit `WorldReal`. Missing fields, wrong types,
missing or extra arguments log an error and return `failed` / `InvalidInput`,
even without terrain. The binding performs no file IO: the first call at a
point returns `loading` while the streamer reads that tile's weight payload,
and later calls answer from the resident payload. Overlay heights do not
change the weights; answers come from the loaded release.

```lua
local ground = world:terrain_layer_weights_at({x=150000.500123, y=10, z=-150000.250321})
if ground.status == 'ready' then
    for _, layer in ipairs(ground.layers) do
        print(layer.name, layer.weight)
    end
end
```

| Result field | Meaning |
|---|---|
| `status` | `ready`, `loading`, `outside_map`, `no_weights`, or `failed` |
| `failure` | `None`, `WrongRelease`, `InvalidInput`, or `LoadFailed` |
| `layers` | Array of `{name, weight}` with nonzero weight, heaviest first, then by name; weights sum to one |
| `releaseId` | Lowercase 16-digit hexadecimal release id |
| `release` | Full identity: hexadecimal `assetId`, `sourceFingerprint`, `releaseId`, numeric `formatVersion` |
| `tile` | `{x,z}` signed tile containing the point, exactly as returned by C++ |

`outside_map` means no instance contains the point. `no_weights` means the
release has no weight record for that tile, or no weights at all, and it never
queues a read.

Each call looks up the World resource again; it caches no release and performs
no IO. A loading state enqueues the C++ query's deduplicated payload request,
which the terrain `TerrainStreamer` reads on a worker after its next per-frame
`Update`; this binding starts no loader itself. An Eager release's payloads are
requested when the scene loads but arrive on their own, so the first frames may
answer `Loading` there too. A Play session keeps its release when `current` changes; app's Edit
safe point replaces the resource after Stop. No resource, an empty resource,
or no instance covering the point gives `OutsideMap` / `None`.

## Rules

**A script-posted voice is handed to `AudioEmitterSystem`, not kept here.**
`world:post_event` writes the playing ID it got back into the emitter's
`mPlayingId`, because that system is the only thing that stops a voice when play
mode ends or the entity is destroyed, and it holds exactly one ID. A voice left
out of it keeps playing past the session with nothing able to reach it. The
consequence is that one emitter carries one voice: a post stops whatever that
emitter was already playing, including its own `eventName`.

**Every audio binding resolves the `AkGameObject` again on each call.** An
`AudioEmitter` has none until `AudioEmitterSystem`'s first update registers one,
so a handle cached at script start would stay 0 for the entity's whole life. An
entity with no emitter, or one not registered yet, gets a refusal — `false`, or
`0` from `post_event` — rather than an error, the same contract the physics
bindings have for an entity with no `RigidBody`.

**Script state never lives in a component.** Components stay POD, so a script's
data lives in the host's instance map, addressed by `instanceRef`.

**Instance state comes from `NewInstanceData()`, never from copying a template
table.** Lua's `{}` literal already gives every instance an independent copy of
any nested table, and a C++-side shallow copy could not do that without
reimplementing a deep copy.

**Callbacks are re-resolved from the code map on every call**, rather than cached
at instance creation. That is what makes a hot-reload reach an already-running
entity on its very next tick, instead of only entities created after the reload.

**Every Lua callback goes through `sol::protected_function`**, never a raw call.
Otherwise a script's runtime error unwinds through C++ frames via Lua's own error
path instead of surfacing as a checked, loggable result. This was verified under
LuaJIT specifically, not assumed to carry over from plain Lua.

**Every structural change a script makes is mid-walk, by construction.**
`ScriptSystem` always calls into Lua from inside its own `ForEach`, so there is
no "outside a walk" case for a script to be in. The bindings route through
`core`'s `DeferredAccess.h` unconditionally rather than trying to detect which
case they are in.

**`ScriptSystem` sets the `ScriptRef` OnRemove hook in `OnStart` and clears it in
`OnStop`.** The hook drops the Lua instance on `RemoveComponent` and on
`DestroyEntity`. A hook left behind calls into freed memory on the next removal.
`ScriptSystem.tests.cpp` covers both the drop and the clear.

**Register native components before loading any script**, including
`RegisterScriptComponents`. `ComponentRegistry` refuses a script that claims an
already-native name, so a script loaded first would win the name instead.

**`Entity` crosses the boundary as a `sol::usertype<Entity>`, never a packed
integer.** A Lua number is a double and exact only to 53 bits, so a packed
`{generation, index}` handle can silently truncate `generation` once a slot has
been reused enough times.

**`FieldKind::Int` accepts any Lua number**, tested with `get_type() ==
sol::type::number` rather than `is<int32_t>()`. That check's strictness is
coupled to the Lua version, and neither variant of it means "is this usable as an
int".

**`world:set` and `world:add` keep a value inside the field's limits**, through
`FieldDesc::Constrain`: an enum `Int` refuses an index with no name, and a
clamped `Float` is clamped to `[mMin, mMax]`, the same limits the Inspector
applies. `world:set` returns `false` for a refusal; `world:add` logs it and
leaves that field at its default. Any `Float` refuses NaN. A `quat` is
normalized, and refused when it has zero length or is not finite, because a
table with a missing key defaults `w` to 1 and the rest to 0.

**`world:each`'s `RuntimeQuery` is cached per term list, as a `World` resource**,
so its lifetime matches the `World`'s. A process-wide cache could alias a reused
heap address across `World` instances, between Catch2 test cases for example.

**`OnStart` and an entity's first `OnUpdate` run in the same `PreUpdate` pass**,
before that phase's flush. A deferred `world:add` issued in `OnStart` is
therefore still pending at the first `OnUpdate`, and becomes visible from the
second.

**LuaJIT was measured, not assumed.** On this build, recursive `fib(32)` is 2.7x
faster than plain Lua, and a 50M-iteration `sin`/`cos` loop is 5.0x faster. The
two benchmarks bracket LuaJIT's bias toward loops over deep recursion.

LuaJIT has no CMake config package, only a `.pc` file, so it is found through
`pkg_check_modules(... IMPORTED_TARGET luajit)`. sol2 detects it via
`LUAJIT_VERSION` in its own headers, so nothing else in this module changed for
the swap.

**The reload poll is a plain accumulator at about one second, not a filesystem
watch.** Nothing else in the engine has async or watch infrastructure to build one
on, and re-running a chunk is cheap enough that polling costs nothing noticeable.

## State

*As of 2026-09-06.* Working: embedding, the full ECS binding, per-entity instance
dispatch with isolated errors and independent state, `CommandBuffer`-deferred
structural changes, and mtime-poll hot reload. All of it has been exercised end
to end against the running app. LuaJIT is the shipped backend.

Tests: `modules/script/tests/`, one file per binding group. The audio one runs
with no sound engine at all — `audio::Init` is never called there — so it covers
the resolve path, the argument checking and the promise that a script on an
entity with no audio keeps running, and nothing about what is audible. Much of the rest was
verified by hand rather than automated — independent per-entity state, a live
edit reaching a running instance, and a deliberate error thrown from inside a
`world:each` callback being caught and logged.

`ScriptRef.scriptName` exists so scene files can round-trip which script an
entity runs. `instanceRef` is this run's live state and is deliberately not
saved. `ScriptResolveSystem` turns the name back into an `instanceRef`, whatever
made the `ScriptRef`.

## Backlog

1. **Cover the hand-verified behaviour.** `ScriptHost` and `ScriptSystem` need
   only a bare `World` and a `sol::state` — no window, no renderer. Binding
   round-trips, per-instance isolation, a deferred mutation issued from inside a
   callback, and the `Entity`-as-userdata precision case are all candidates.
2. **`world:each` builds a fresh Lua table per matched entity per term, every
   call**, even though the `RuntimeQuery` itself is now cached. Whether that
   allocation is worth avoiding is unmeasured.
3. **No script-facing default for a native component's field**, the way
   `NewInstanceData()` gives one for a script's own state. `world:add` with no
   fields table uses whatever `ComponentOps` registered, which is a C++-side
   decision a script cannot override per call site.
4. Confirm the LuaJIT and plain-Lua swap stays a `vcpkg.json` entry plus one
   CMake block, across a future sol2 upgrade that changes how it detects the
   flavour underneath.

## Changed

*2026-11-20* — **Bridges can call callbacks of their own, and scripts control
the session.** Was: a script got `OnStart`, `OnUpdate`, `OnFixedUpdate` and
`OnStop` only, and could not pause or quit. Now: `CallScriptCallback` and
`LuaState` in `BindingApi.h`, which `gameui_script` uses for `OnUI`, and
`world:pause/resume/restart/quit/is_paused`. Because: a game draws its own UI
and pause menu from Lua. `IScriptHost` is unchanged.

*2026-10-27* — **Exposure, focus distance, aperture and shutter strength
left the post-processing bindings (#231).** Was: `set_hdr`/`get_hdr` carried
`exposure`, `set_depth_of_field`/`get_depth_of_field` `focusDistance` and
`aperture`, and `set_motion_blur`/`get_motion_blur` `shutterStrength`. Now:
they are `Camera` fields reached through `world:set`/`world:get`; the `get_*`
tables no longer have them, and passing one to a `set_*` refuses the call with
an error naming the Camera field. Because: the post effects read them from the
active camera, and a key that silently did nothing would hide the move. No
script in `games/` used them.

*2026-10-25*, **`world:set` takes only the documented forms.** Was: a vector, color or rotation table with a missing or misspelled key took 0 for it (1 for a rotation's `w`), and NaN or infinity went into a number field. Now: every key must be a number, NaN and infinity are refused, a number field keeps to the range it declares, a choice from a list must be in the list, and the refusal is one error that names the field and what was expected, with the field left as it was. A rotation may leave out `w`, which is 1. Because: a typo should fail loudly instead of becoming a zero, and a script should not be able to put a field where the editor could not. No script in `games/` wrote a partial table, so none changed.

*2026-10-25*, **`OnFixedUpdate`, and a `ScriptRef` starts clean when it is added.** Was: scripts
had no callback on the physics step, so a force set in `OnUpdate` landed on whichever step came
next, and a `ScriptRef` copied from another entity carried that entity's `instanceRef` and
`started`. Now: `ScriptFixedSystem` runs `OnFixedUpdate` before each physics step, and the
`ScriptRef` OnAdd hook resets `instanceRef` and `started`. Because: physics work belongs on the
fixed step, and a copy must not share the original's instance.
*2026-10-25* — **Script writes respect field limits; `world:mesh_bounds` and
`world:is_playing` added (#219).** Was: `world:set` and `world:add` wrote any
value of the right shape, so a script could set `Light.type = 9`, a negative
`Light.intensity` or a NaN, and `{ y = 1 }` stored a non-unit rotation; a script
could not read a mesh's bounds or ask whether an emitter was still playing. Now:
writes go through `FieldDesc::Constrain`, quaternions are normalized, and the
two bindings exist. Because: the Inspector's limits only guarded the Inspector,
and the #219 audit found these as the gaps a script could not work around.

*2026-10-24* — **A live resolver, `ScriptModule`, and the bindings moved to
bridges.** Was: `app`'s `ResolveSceneScripts` ran after a scene load, a Stop, a
duplicate, a restore, a prefab publish and an import, and a new path that made a
`ScriptRef` had to remember the call; `ScriptHost`'s constructor registered every
library's bindings, so `script` linked audio, input, physics, renderer and
terrain. Now: `ScriptResolveSystem` resolves every frame, a rename replaces the
instance, `ScriptModule` owns the host and its systems, and five bridge
libraries add their bindings through `AddBindings`. Because: a forgotten resolve
left a script silently dead, and a library's Lua surface belongs next to it, not
inside `script`.

*2026-10-19* — **`world:set_motion_blur/get_motion_blur` joins the
post-processing bindings (#212).** Was: the five effects above. Now: Motion
blur followed the exact same shape - a sixth `PostEffectRegistry` entry, two
more functions in `PostProcessingBindings.cpp` reading/writing through
`ReadMotionBlurParams`/`WriteMotionBlurParams`, clamping `shutterStrength` to
`>= 0` and `sampleCount` to `>= 1` the same way the others clamp their own
fields. Because: same reasoning as Film's entry below - a post-processing
effect the panel exposes and gameplay code can't reach is the gap this file
exists to close.

*2026-10-19* — **`world:set_film/get_film` joins the post-processing
bindings (#213).** Was: the four effects below. Now: Film followed the exact
same shape the moment it existed as a fifth `PostEffectRegistry` entry - two
more functions in `PostProcessingBindings.cpp` reading/writing through
`ReadFilmParams`/`WriteFilmParams`, clamping grain/contrast/saturation/
vignetteStrength/vignetteRadius to `>= 0` the same way `set_bloom` clamps its
own fields. Because: same reasoning as DoF's entry below - a post-processing
effect the panel exposes and gameplay code can't reach is the gap this file
exists to close.

*2026-10-18* — **`world:set_depth_of_field/get_depth_of_field` joins the
post-processing bindings.** Was: the three effects below. Now: DoF followed
the exact same shape the moment it existed as a fourth `PostEffectRegistry`
entry - no new binding mechanism, just two more functions in
`PostProcessingBindings.cpp` reading/writing through `ReadDepthOfFieldParams`/
`WriteDepthOfFieldParams`. Because: a post-processing effect the panel exposes
and gameplay code can't reach is the gap this file exists to close - leaving
the newest effect out would have been the one exception.

*2026-10-18* — **Post-processing bindings: `world:set_fog/get_fog`,
`set_hdr/get_hdr`, `set_bloom/get_bloom`.** Was: Fog, HDR and Bloom
were only reachable from the editor's Post-Processing panel - no script could
read or change any of them, not even the ones (Fog, HDR) that predate this
module's own post-processing work. Now: `PostProcessingBindings.cpp`, a new
binding file alongside `AudioBindings`/`PhysicsBindings`/`TerrainBindings`,
extends `World` the same way they do. `PostProcessingSettings` is one global
resource, not a `Camera` field, so - like `set_state` for the one global Wwise
state - these take no entity. `HdrEffect.h` gained `ToString`/`ParseOperator`
(previously private to `HdrEffect.cpp`) so the binding's `tonemapOperator`
string and the panel's own json use the exact same mapping; everything else
reads/writes through the existing `Read*Params`/`Write*Params` functions
unchanged. This module now depends on `mir::renderer` (private) - its first
reach into rendering, scoped to this one settings resource, not the renderer's
own runtime state. Because: every post-processing effect the panel exposes
should be reachable from gameplay code too.

*2026-10-02* — **Input bindings read `InputState::ForGame`.** Was: `Get`, so a
script saw every press, including those that navigate an open pause menu. Now:
at rest while the game UI holds the input. Nothing else changes for a script.

*2026-09-05* — **Asset copying was fixed to run on an asset-only change.** Was: a
`POST_BUILD` command in `cmake/Assets.cmake` that shared its target's up-to-date
check, so ninja skipped the copy whenever nothing needed relinking. A script edit
would cook fine and then never reach the running executable's `cooked/`
directory, which made hot reload look broken. Now: a separate `OUTPUT`-based
custom command keyed off the cook stamp.

`Shaders.cmake` and `Fonts.cmake` still use the old pattern and probably have the
same gap.
