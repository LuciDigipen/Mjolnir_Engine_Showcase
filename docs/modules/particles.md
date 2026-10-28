# particles

CPU effect definitions and bounded emitter playback. `particles` links core and
assets, its JSON parser is private. It needs no renderer, Vulkan device, editor or
Lua state. `particles_script` adds the Lua controls separately.

`mir::particles` · API `modules/particles/include/particles/` · maintainer Sumin Park · reviewed 2026-10-27

## Ownership and frame flow

`ParticlesModule` registers the `ParticleSystem` component, its hooks and lifecycle
stages, and creates the `ParticleWorld` world resource. App constructs it after
assets. `ParticleWorld` owns CPU system and emitter instances. Components contain
only authored names, playback intent, and optional user parameter overrides.
`particles_runtime` translates runtime emitters to render data for `particles_gpu`.

The LateUpdate system runs at order 70, after transform propagation. During
running Play it gathers each system's `WorldTransform::Pose()` without narrowing
world coordinates, resolves its asset and emitter definitions, and advances each
emitter instance's playback clock.
Edit mode and global Play pause freeze updates. Each instance can also be paused.
The clock accepts 60 Hz ticks, at most four per update, and counts dropped hitch
time instead of retaining a catch-up queue.

## Authoring and cooking

Add a `ParticleSystem` and a normal Transform to an entity. `systemName` names a
`.particle.json` system asset relative to the game's asset root. `playOnStart`
defaults to true. Optional reflected `spawnRateScale` and `sizeScale` overrides
are enabled with `overrideParameters`. Handles and playback flags are transient,
including when a component is copied, a scene is loaded or Play is restored. No
particle editor widgets or preview are provided yet.

AssetCooker recognizes the exact suffix `.particle.json`. All fields below are
required and unknown fields are rejected:

```json
{
  "version": 2,
  "emitters": ["exhaust.emitter.json"],
  "parameters": {"spawnRateScale": 1.0, "sizeScale": 1.0}
}
```

A system lists at most eight unique emitter asset paths and may set finite scale
parameters in [0, 16]. Each `.emitter.json` version 1 defines target (`gpu` or
`cpu`), lifetime, spawn rate, capacity, texture, and optional renderer defaults
(size, color and velocity). Emitter and texture paths resolve from the asset root
and must stay inside it. Textures use the typed texture cooker. Missing or invalid
dependencies fail before manifest publication. System blobs use type tag
`0x54524150`, version 2; emitter blobs use type tag `0x454D4954`, version 2.
Cooked system assets retain emitter IDs, while loaded system definitions resolve
those emitter and texture assets through `AssetCache`. Runtime never reads source
JSON or decodes images.

## Playback API

Obtain `ParticleWorld` from the World resource or `ParticlesModule::Particles()`.
Entity controls return false for an entity without a `ParticleSystem`.

| Operation | Result |
|---|---|
| `Start(entity)` | Accepts a system emission request. Resolution/allocation happens in the next running LateUpdate. A draining instance resumes with its existing identity. |
| `Stop(entity)` | Stops emission, retaining the instance for its configured maximum lifetime. Repeated stops do not extend the drain. |
| `Pause(entity, bool)` | Holds or releases this instance's playback clock, including its drain. |
| `Reset(entity)` | Destroys the current instance, clears pause, then applies `playOnStart` again at the next update. |
| `Destroy(entity)` | Destroys immediately and suppresses automatic restart until another start/reset request. The component remains. |
| `Find(entity)` | Returns the currently owned system instance, or null. Do not retain pointers across mutation or updates. |

A successful start request does not guarantee an allocation. Bad assets log once
per failed asset reference until an explicit retry, reference change or reload.
Excess emitters wait for capacity and retry on later updates. The default pool
holds 64 logical instances, a constructor may select 0 through 64. Direct runtime
starts reject overflow and increment `rejectedStarts`. `active`, accepted `ticks`
and `droppedSeconds` are also available from `Runtime().Stats()`.

Handles contain a slot index and generation. Destroy/clear invalidate old
handles, exhausted generations retire the slot instead of wrapping. Only authored
fields are registered for serialization. Changing a nonempty effect reference
replaces its old instance, clearing the reference releases it immediately.

## Lifecycle and reload

Removing the component or its owner detaches the instance at its last world pose,
stops emission and unpauses it for a bounded drain. No later access to the removed
entity is required. SceneUnload, SceneLoad, PlayBegin and PlayEnd clear all
instances and transient component state, preserving handle invalidation.

ReloadPrepare validates every unique live effect and its dependencies in the
candidate cache without changing playback. Failure aborts the transaction,
ReloadAbort keeps existing handles and definitions. After assets commit,
ReloadCommit clears old instances and transient bindings while preserving each
emitter's stopped/paused intent. Requested emitters resolve the new definition at
the next running update. Detached drains end at a successful reload.

`particles_script` binds `world:particle_system_start/stop/pause/reset/destroy/state`
and `particle_system_parameters`. States are `active`, `inactive`, `complete`,
and `paused`.

## Verification and limits

`engine_particles_tests` covers schema/blob/dependency rejection, stale handles,
capacity, stop versus destroy, clocks, copied components, owner removal,
scene save/load, repeated Play transitions and reload abort/commit. These tests
need no Vulkan device. `engine_particles_script_tests` exercises the Lua controls
against the same headless resource. `gltf_importer_tests` includes a real
AssetCooker round trip from `tools/AssetCooker/tests/particle_fixtures/`, with
failure checks for missing textures and unsupported source versions.

The CPU drain timer remains conservative, it does not wait for GPU alive counts.
Trails, moving-emitter page allocation, textured effects, sorting, lighting,
inspector preview and performance acceptance remain later tasks.

## GPU bridge (T2a)

`particles_runtime` depends on `particles` and `particles_gpu`. Its LateUpdate
bridge translates each live GPU emitter instance into stateless render data,
including its own generation, capacity, fixed-step seconds, and scheduled births.
Fractional births are accumulated by CPU emitter instances. CPU-target emitters
are not submitted to this GPU backend.

The `particles_gpu` module owns persistent records, fixed per-emitter ranges,
compute dispatch, counters/readback, and sprite drawing. The renderer owns HDR
composition images, synchronization, and pass order through a generic `IFxSystem`
registration boundary. The post-processing stack owns no particle state. FX
simulation runs once after the frame slot wait, independent of view count. Draws
run inside renderer-owned HDR composition after opaque effects and before bloom.

The backend keeps 65,536 80-byte records, accepts at most 4,096 births per frame,
and supports 64 emitter ranges. Each dispatch compacts survivors, adds births,
and writes indirect draw counts. Same-queue barriers protect range reuse and make
compute output available to indirect drawing and readback. Only completed frame
slot counters are exposed. The current backend is a bounded GPU sprite foundation,
not a performance sign-off; it does not yet render cooked textures, and particles do not participate in reflections, shadows
or GI.

`ParticleGpuCheck` uses the production API to verify empty/full pools, steady
state, pause/drain, burst expiry, recycling, multiple views, all 64 emitters,
generation replacement, range exhaustion, resize, unload, and two GPU emitters
owned by one system. `gltf_importer_tests` covers system/emitter asset cooking,
changed emitter recooks, dependency failure, and preserving manifest entries
from other roots during particle-only recooks.

## Cooked module stacks

Emitter source version 1 accepts ordered `spawnModules` and `updateModules` arrays.
Each entry has a `name` and a numeric `parameters` array. The initial GPU registry
supports spawn `Initialize` (no parameters) and update `Age`, `Integrate` (no
parameters), and `Acceleration` (three finite floats, metres per second squared).
Omitted GPU stacks default to Initialize and Age/Integrate, reproducing the original
pool behavior. CPU stacks default to empty, these initial GPU modules reject CPU
targets. CPU simulation modules are introduced with the CPU emitter implementation.
Stacks are bounded to 32 entries per stage and execute in authored order.

AssetCooker composes Slang from the registered operations and invokes the configured
`slangc` process, using SPIR-V 1.5 and the `computeMain` entry point. Parameters for
module operations are compiled constants. Per-frame values use a generated, versioned
80-byte input layout. Script and layout blobs are typed dependencies of emitter
content version 2. Equal stacks and operation parameters share one script regardless
of per-emitter renderer values, capacity, or spawn rate. Compilation and validation
must succeed before the new manifest is published. Partial recooks retain other
roots and remove unreferenced script/layout manifest entries.

Runtime asset loading validates script type, version, SPIR-V framing and the exact
supported input layout, then supplies cooked bytes through the scene proxy. The
game never invokes Slang or reads emitter source JSON.
