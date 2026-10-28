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
The separate `particles_runtime` bridge submits GPU sprites through renderer-owned APIs,
there is no per-particle CPU simulation.

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

The CPU drain timer remains conservative, it does not wait for GPU alive counts.
Trails, moving-emitter page allocation, textured effects, sorting, lighting,
inspector preview and performance acceptance remain later tasks. The temporary
[composition probe](../particle-composition.md) remains a T0 regression reference,
its final production removal gate still applies.

## GPU bridge and pool (T2)

`particles_runtime` depends on particles and renderer, neither library depends on
the bridge. App constructs it after both libraries. Its LateUpdate system at order
80 follows particle collection at 70. It preserves fractional birth counts and
submits the last accepted CPU ticks, capped at four 60 Hz steps. Global or instance
pause submits zero time and births. Scene/play/reload cleanup uses the logical
handles from the CPU runtime, omitted generations disappear from the next snapshot.
Renderer scene release also clears the pending snapshot explicitly.

`VulkanRenderer::SubmitParticles` copies one immutable snapshot with up to 64
instances. Each submission replaces the pending snapshot, omitted indices retire,
and an empty snapshot clears all instances. Multiple updates before a successful
render retain only the latest packet, superseded births are counted and no
catch-up queue grows while rendering is skipped. Without another submission,
existing instances remain frozen with zero births. Simulation runs once at
BeginFrame, independently of the number of views and raster versus RT mode.

The renderer owns 65,536 persistent 80-byte records and first-fit per-instance
ranges. A generation/capacity replacement resets only that range. One compute
invocation per emitter compacts survivors in spawn order, recycles the free suffix,
adds births and writes a 32-byte counter/indirect record. This deliberately simple
foundation has serial work inside each range, scalability is not signed off.
A frame accepts at most 4,096 births, range and live-capacity exhaustion drop work.
`ParticleStats()` exposes timeline-completed alive/spawn/drop counters plus CPU
rejection and dropped-submission totals. Only 2 KiB of counters are copied per
frame, read when that frame slot's existing timeline wait completes.

Both frames share the persistent records. Same-queue barriers order earlier
compute, indirect, vertex and transfer accesses before range reuse, then make
compute writes available to indirect drawing, vertices and counter copies.
Inputs, graphics/compute descriptor sets, readback and HDR outputs are per frame
slot. Reusing a range requires no host wait, buffers remain allocated until normal
renderer shutdown after its existing device-idle wait. Resize changes the HDR
images through the renderer's existing resize synchronization.

The initial unlit radial sprites composite after opaque DoF/motion blur and RR,
before bloom and tonemapping. They sample opaque reversed-Z depth in raster and
ray distance in RT, with the existing jitter compensation. Raster views draw
separately, RT retains the engine's single-view restriction. A translation-only
anchor stays fixed for each range, new births beyond its 1,024 m radius are dropped,
and escaped particles retire. T3 supplies moving-emitter pages and T5 supplies
production depth/lighting/material/sorting proof. This foundation does not render
the cooked texture yet and its sprites do not participate in reflections, shadows
or GI.

`ParticleGpuCheck` is built only with BUILD_TESTING. It uses the production
submission API to verify empty/full pools, steady state, pause/drain, finite burst
expiry, recycling, multiple views, all 64 instances, generation replacement,
range exhaustion, actual window resize and unload. It checks that steady-state
idle-wait counters stay unchanged. Run `ctest -R smoke.particle-gpu` in a configured
build, or `ParticleGpuCheck capture.png raster|rt|dlss`. If RenderDoc is installed,
launch it with `renderdoccmd capture -w ... ParticleGpuCheck capture.png raster
capture` to request a GPU capture. Capture API support is confined to the test
executable, the game has no RenderDoc dependency.
