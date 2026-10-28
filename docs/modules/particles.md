# particles

CPU effect definitions and bounded emitter playback. `particles` links core and
assets, its JSON parser is private. It needs no renderer, Vulkan device, editor or
Lua state. `particles_script` adds the Lua controls separately.

`mir::particles` · API `modules/particles/include/particles/` · maintainer Sumin Park · reviewed 2026-10-27

## Ownership and frame flow

`ParticlesModule` registers `ParticleEmitter`, its component hooks and lifecycle
stages, and creates the `ParticleWorld` world resource. App constructs it after
assets. `ParticleWorld` resolves cooked assets and owns `ParticleRuntime`, which
holds a fixed-capacity logical instance pool. Components contain no allocations.
There is no per-particle CPU simulation or rendering yet.

The LateUpdate system runs at order 70, after transform propagation. During
running Play it gathers each emitter's `WorldTransform::Pose()` without narrowing
world coordinates, resolves requested effects and advances the playback clock.
Edit mode and global Play pause freeze updates. Each instance can also be paused.
The clock accepts 60 Hz ticks, at most four per update, and counts dropped hitch
time instead of retaining a catch-up queue.

## Authoring and cooking

Add a `ParticleEmitter` and a normal Transform to an entity. The two persisted
fields are `effectName`, a path relative to the game's asset root, and
`playOnStart`, which defaults to true. Handles and playback flags are transient,
including when a component is copied, a scene is loaded or Play is restored.
Normal component reflection supplies field serialization, no particle editor
widgets or preview are provided yet.

AssetCooker recognizes the exact suffix `.particle.json`. All fields below are
required and unknown fields are rejected:

```json
{
  "version": 1,
  "lifetime": 2.0,
  "spawnRate": 60.0,
  "capacity": 256,
  "texture": "textures/exhaust.png"
}
```

`lifetime` is finite seconds in (0, 120], `spawnRate` is a finite particles/second
value in [0, 65536], and `capacity` is an integer in [1, 65536]. These are
definitions for future simulation, they do not allocate particle buffers today.
Texture paths are relative to the asset root, rather than the effect file, and
must remain inside that root. PNG and JPEG dependencies use the ordinary typed
texture cooker. A missing or invalid dependency fails the cook before publishing
a replacement manifest.

`BuildParticleEffectAsset` wraps the validated definition in the standard asset
blob, type tag `0x54524150`, content version 1. The cooked definition contains a
texture AssetId rather than its source path. `LoadParticleEffect` validates the
blob type/version, schema and typed texture dependency through `AssetCache`.
Runtime never opens source JSON or decodes source images.

## Playback API

Obtain `ParticleWorld` from the World resource or `ParticlesModule::Particles()`.
Entity controls return false for an entity without a `ParticleEmitter`.

| Operation | Result |
|---|---|
| `Start(entity)` | Accepts an emission request. Resolution/allocation happens in the next running LateUpdate. A draining instance resumes with its existing identity. |
| `Stop(entity)` | Stops emission, retaining the instance for its configured maximum lifetime. Repeated stops do not extend the drain. |
| `Pause(entity, bool)` | Holds or releases this instance's playback clock, including its drain. |
| `Reset(entity)` | Destroys the current instance, clears pause, then applies `playOnStart` again at the next update. |
| `Destroy(entity)` | Destroys immediately and suppresses automatic restart until another start/reset request. The component remains. |
| `Find(entity)` | Returns the currently owned instance, or null. Do not retain pointers across mutation or updates. |

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

`particles_script` binds `world:particle_start(entity)`, `particle_stop`,
`particle_pause(entity, bool)`, `particle_reset`, `particle_destroy` and
`particle_state`. State is `stopped`, `emitting`, `draining` or `paused`. A pending
request with no allocated instance reports `stopped`.

## Verification and limits

`engine_particles_tests` covers schema/blob/dependency rejection, stale handles,
capacity, stop versus destroy, clocks, copied components, owner removal,
scene save/load, repeated Play transitions and reload abort/commit. These tests
need no Vulkan device. `engine_particles_script_tests` exercises the Lua controls
against the same headless resource. `gltf_importer_tests` includes a real
AssetCooker round trip from `tools/AssetCooker/tests/particle_fixtures/`, with
failure checks for missing textures and unsupported source versions.

No GPU simulation, visible sprites, trails, history, renderer bridge, minimized
rendering policy, inspector preview or performance signoff is implemented here.
Drain is a conservative lifetime policy, not a measured GPU alive count. The
temporary [composition probe](../particle-composition.md) remains separate and
must be removed when production rendering replaces it. Raster and ray-tracing
composition, followed by reflections/shadows/GI, remain rendering work.
