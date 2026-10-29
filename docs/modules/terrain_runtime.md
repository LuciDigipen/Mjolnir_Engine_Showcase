# terrain_runtime

Streams, draws and collides terrain at runtime. It is the one place terrain meets
the renderer and physics, so `terrain` itself, its tests and `TerrainTool` need
neither Vulkan nor Jolt.

`mir::terrain_runtime` · depends `mir::terrain`, `renderer`, `physics`, `project`, `core`, all public · API `modules/terrain_runtime/include/terrain_runtime/` · maintainer Jackey Cen · reviewed 2026-10-24

## Model

A bridge library: moving this code into `terrain` would put Vulkan and Jolt in
`terrain`'s link closure, and into `renderer` or `physics` would make either
depend on `terrain`. `TerrainRuntimeModule` builds a `TerrainRuntime`, binds it
to the `World`, the renderer and the game, and registers its frame and stage
work.


A scene holds terrain as `TerrainInstance` components; the `TerrainWorld` World
resource holds what they resolve to (see [terrain.md](terrain.md)).
`TerrainRuntime` owns everything derived from it, shared by every instance of a release: the height field
pieces a `TerrainCollisionResidency` builds for the bodies that need them
(#128), and, shared by every viewport, the GPU meshes of the draw units the
viewports select (#125).

```
constructor    -> TerrainRuntime::Bind      streaming settings -> TerrainStreamingSettings, TerrainStreamer resources;
                                            renderer mesh streaming; collision residency; registers itself in the
                                            ViewDrawSources and PhysicsStepGates resources
FrameBegin     -> TerrainRuntime::Update    TerrainStreamer::Update, TerrainReleaseSwitch::Update,
                                            TerrainMeshResidency::Install, TerrainCollisionResidency::Install: the
                                            install point; outside Play the bodies' collision needs too; each staged
                                            release's prewarm reported to the switch
FrameBegin, Edit -> Refresh(dt)             an edit settled 0.5 s; current pointers polled every 1 s (a newer release
                                            of a loaded asset starts a background check); a ready switch adopted
SceneLoad      -> Refresh(force)            after an Unload, CollectTerrainScene and ResolveTerrainWorld (Async,
                                            keepLoaded) for the whole spec; after Stop's restore, almost nothing
PlayBegin      -> Refresh(force), BeginPlay adopt a ready switch, then Play pins what is loaded; a switch in progress
                                            waits for Stop
SceneUnload    -> Unload                    meshes forgotten, collision pieces removed, TerrainWorld emptied
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
streamer with a `TerrainJobExecutor` background lane on the app-owned executor,
also used for mesh/collision builds and release checks. `io.workerThreads` caps
terrain's share of the engine workers; `--workers 0` runs inline. The adapter
lives in `terrain_runtime/TerrainJobExecutor.h`; `terrain` only knows the executor
interface. Runtime shutdown waits for submitted callbacks before destroying
collaborators, then releases shared ownership without closing the lane. A retained
streamer can keep posting work; only the last owner closes the lane.
The `gpu` section
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
and `TerrainGpuUploader` (`TerrainRender.h`) queues it with the
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

## API

| Type | Header | Role |
|---|---|---|
| `TerrainRuntimeModule` | `terrain_runtime/TerrainRuntimeModule.h` | The module: binds the runtime, registers its `FrameBegin` work and its stage functions |
| `TerrainRuntime` | `terrain_runtime/TerrainRuntime.h` | Streaming, residencies, release switching, per-viewport drawing, the physics step gate |
| `TerrainGpuUploader` | `terrain_runtime/TerrainRender.h` | Queues a built terrain mesh with the renderer |

## Rules

**Terrain meshes do not survive `ReleaseSceneResources`.** It frees every mesh,
queued and retiring ones too, and only a scene change runs it now. The terrain
`SceneUnload` function, `Unload`, forgets every handle; whether it runs before
or after the renderer's free does not matter, since handles are generational and
a stale one is skipped. Stop keeps the meshes, because they are keyed by
release and the restored scene names the same ones. Dropping terrain while the scene
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
runs in the `FrameBegin` slot of an Edit frame, at `PlayBegin` and at the
`SceneLoad` that ends Stop, never during Play: one Play keeps the releases it entered
with, and a mesh, query or collision piece of another release never shares a
frame or a physics step with them. A staged release is selected, read, meshed and
built in Edit, but never drawn or placed, and Play drops its staging.

## State

*As of 2026-10-24.* Moved out of `app` unchanged apart from its triggers: the
module's `FrameBegin` slot and stage functions replace `App`'s calls, and
`TerrainRuntime::Load` went, since `Refresh(0, true)` after an `Unload` applies
the whole spec. Tested through `terrain` (`TerrainScene`, `TerrainCollider`,
`TerrainCollisionResidencyJolt`, `TerrainReleaseSwitch`) and the
`smoke.TerrainCheck` runs; see [app.md](app.md#state).

## Backlog

1. Split into a render bridge and a physics bridge if a build ever wants one
   without the other. No headless target needs that today.

## Changed

*2026-10-24* — **Moved out of `app` into a bridge library.** Was:
`src/app/TerrainRuntime` and `TerrainRender`, private to `app`, called by
`App`'s frame loop and scene transitions. Now: a library of its own with a
module that registers that work itself. Because: `App` no longer wires each
library by hand, and `terrain` must stay free of Vulkan and Jolt.
