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
  "version": 3,
  "parameters": {"spawnRateScale": 1.0, "sizeScale": 1.0},
  "placements": [{"id": 1, "emitter": "exhaust.emitter.json", "offset": [0, 0, 0]}]
}
```

A system lists at most eight placements. A placement has a unique id, an emitter asset
path and a local offset, and the same emitter asset may be placed more than once.
Scale parameters are finite and in [0, 16]. Each `.emitter.json` version 3 defines target (`gpu` or
`cpu`), lifetime, spawn rate, capacity, texture, and optional renderer defaults
(size, color and velocity). Emitter and texture paths resolve from the asset root
and must stay inside it. Textures use the typed texture cooker. Missing or invalid
dependencies fail before manifest publication. System blobs use type tag
`0x54524150`, version 3; emitter blobs use type tag `0x454D4954`, version 7.
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

## Cooked behavior stacks

Emitter source version 3 accepts `spawnBehaviors` and `updateBehaviors` arrays.
Each entry has a `name`, a numeric `parameters` array and an `enabled` flag. A behavior
appears at most once, behaviors run in the registry's fixed order, and behaviors in
one mode group exclude each other. A required behavior cannot be disabled. The initial GPU registry
supports spawn `Initialize` (no parameters) and update `Age`, `Integrate` (no
parameters), and `Acceleration` (three finite floats, metres per second squared).
Omitted GPU stacks default to Initialize and Age/Integrate, reproducing the original
pool behavior. GPU operations reject CPU targets. CPU ribbons require the stack described below.
Stacks are bounded to 32 entries per stage and execute in authored order.

AssetCooker composes Slang from the registered operations and invokes the configured
`slangc` process, using SPIR-V 1.5 and the `computeMain` entry point. Parameters for
behavior operations are compiled constants. Per-frame values use a generated, versioned
160-byte input layout. Script and layout blobs are typed dependencies of emitter
content version 6. Equal stacks and operation parameters share one script regardless
of per-emitter renderer values, capacity, or spawn rate. Compilation and validation
must succeed before the new manifest is published. Partial recooks retain other
roots and remove unreferenced script/layout manifest entries.

Runtime asset loading validates script type, version, SPIR-V framing and the exact
supported input layout, then supplies cooked bytes through the scene proxy. The
game never invokes Slang or reads emitter source JSON.

## Moving emitters and tiles

The CPU world manager schedules bounded GPU birth records from time rate,
`spawnPerMeter`, and one initial `burst`. Fractional time and distance remainders
persist per emitter. Burst, time, then distance consume the shared 4,096-birth
budget, accepted births are ordered by their fraction along the swept attachment
span. Sequence numbers also advance over dropped work. Scheduling belongs to the
CPU instance, cooked GPU modules initialize and simulate the accepted records.

`localOffset` is transformed by the propagated pose. The difference between
accepted attachment poses includes translation and rotation, divided by elapsed
input time to give world-relative attachment velocity. `inheritVelocity` scales
this velocity before adding it to the transformed authored velocity. `Drag`
converges world velocity toward the supplied world-space `wind`, wind is not
added a second time. Wind is an optional system user parameter and reflected
ParticleSystem override, bounded to 4,096 m/s per axis.

Ordinary clock remainders preserve the complete sweep, including 90/144 Hz input.
Hitches simulate at most four 60 Hz ticks, emitting along the final accepted
portion of motion. Teleports beyond `teleportDistance` collapse the emission span,
zero inherited velocity and reset fractional distance, avoiding a connecting
streak. Stopped and paused motion does not replay on resume. Bursts are once per
new instance, resuming a drain does not replay them.

All emitters in a system share a world-precision tile origin. When the system is
more than 64 m away, the next accepted tick moves the tile to the system position.
The GPU dataset subtracts old and new origins in world precision, then translates
every survivor in its regular update dispatch. Per-view tile-to-camera subtraction
also precedes narrowing. There is no extra rebase dispatch. Live GPU positions
have a 65,536 m local spatial budget, float spacing remains below one centimetre
inside it. Particles outside that budget retire. Float-world builds reject system
and attachment coordinates beyond 65,536 m on any absolute axis, rather than
claiming large-world precision.

Additional GPU operations are spawn `VelocityVariation` (three nonnegative velocity
amplitudes), and update `Drag` (coefficient), `SizeOverLife` (end size multiplier),
`FadeOverLife` (end alpha), and `Flipbook` (integer columns, integer rows, frames per
second). Curves interpolate linearly with normalized age and behaviors run in
the registry order. Seeded variation hashes the emitter seed and birth sequence, it
promises repeatability for the same stream/timing, not cross-GPU bitwise identity.
Flipbooks sample the cooked texture atlas.

## CPU ribbon emitters

CPU emitters use the same stack editing as GPU emitters, and the CPU registry is
`RibbonSpawn(maxInterval)` for the spawn stage plus `RibbonAge`, `RibbonWind`,
`RibbonWidth(endScale)`, `RibbonWidthCurve(start, middle, end)`, `RibbonColorGradient`
and `RibbonFade` for the update stage. `RibbonSpawn` and `RibbonAge` are required,
everything else is optional and each behavior appears at most once. Behaviors
assign the attributes they own. `RibbonColorGradient` assigns the whole color, then
`RibbonFade` scales the gradient alpha by the remaining life. Without a width behavior the width is the emitter size, without a gradient the
color is the emitter color, without `RibbonFade` alpha stays constant and without
`RibbonWind` samples do not drift. The stack is resolved once per tick, so the cost
does not grow per sample. This focused stack has no GPU script dependency.
Set `spawnRate` and `burst` to zero, use
`spawnPerMeter` for distance spacing. A point is emitted at whichever comes first,
distance spacing or maximum interval (1/60 to 10 seconds). Zero spawnRateScale
stops emission and breaks the ribbon while existing samples continue simulating.

Each sample stores a WorldPosition, simulation birth time, ribbon ID, monotonic
link order and accumulated texture distance. World-space wind advects samples,
width follows the width behavior, alpha fades linearly to zero when `RibbonFade` is present. The model prescribes
wind and does not simulate wake or fluid dynamics. Float-world samples that drift
beyond the supported absolute range retire with a rejection counter.

Teleports, stop/resume and pause/resume start new ribbon IDs. Removed owners drain
unpaused under the established lifetime policy. Scene/play/reload resets discard
history. Offscreen samples still age and drift. Eviction removes the oldest
sample without changing remaining texture coordinates or link IDs.

At most 1,024 samples belong to each of 64 emitter instances (65,536 total).
CPU and GPU scheduling share the 4,096 birth-work budget, overflow is counted and
never queued. Runtime stats expose evicted and rejected ribbon points. State tests
compare constant-wind straight flight within 0.1 mm near the origin and within
1 cm after a 1,000 km translation, and retain old history after the aircraft moves
another 1,000 km. Geometry tests independently return a camera to that history.

## Renderer properties

Emitter content version 5 carries the validated `renderer` object through cooking
and the immutable scene bridge. Renderer choices do not change the cooked simulation
script identity. Existing size, color and velocity fields remain supported.

| Property | Values/default | Meaning |
|---|---|---|
| alignment | screen (default), velocity | Sprite axes, ribbons follow their tangent |
| facing | plane (default), position | Camera plane or direction toward each point |
| blend | alpha (default), additive | Premultiplied HDR blending |
| sort | backToFront (default), none | Per-view element ordering |
| softDistance | 0.25 m, 0 to 1,024 | Opaque intersection fade, zero selects a hard cut |
| maxDistance | 65,536 m, greater than zero through 10,000 km | Conservative draw culling, simulation continues |
| lighting | unlit (default), sunAmbient | Basic two-sided sun diffuse and ambient |
| fog | true | Existing renderer distance fog |
| textured | false | Sample the cooked texture, Flipbook samples its atlas regardless |

Sprites and trails share the GPU material path. Per-view geometry/sort buffers
never change WorldPosition ribbon history or GPU simulation records. See
[particles_gpu](particles_gpu.md) for depth reconstruction, composition order and
remaining glass/AA/post-effect limits.

## Authoring and quality

Project > Particles edits system references, user scales and world wind, emitter
spawn/update stacks, renderer properties and importance/distance quality. Save
buttons write validated source JSON atomically and queue the normal AssetCooker
workflow. GPU stack edits compile new scripts, renderer-only edits retain script
identity. Emitter and system source are version 3, cooked system content is
version 3 and cooked emitter content is version 7.

`SizeCurve(start,middle,end)` and `RibbonWidthCurve(start,middle,end)` interpolate
linearly at normalized ages 0, 0.5 and 1. `ColorGradient` and
`RibbonColorGradient` take two RGBA endpoints, multiplying the emitter base color.
GPU ColorGradient supplies alpha in place of the default fade. CPU RibbonFade
remains last and multiplies gradient alpha. The ribbon stack permits RibbonWidth
or RibbonWidthCurve and an optional RibbonColorGradient before RibbonFade.
CPU ribbons prepare the emitter's width/gradient parameters once per tick before
evaluating sample ages, including while draining after emission stops.

Preview uses a normal cooked system and the production runtime in Edit. Pause
freezes its simulation, Clear, closing the panel, scene changes and Play discard
it. Successful asset reload discards old history and restarts an active preview,
preserving its pause choice. Failed cooking or reload keeps the last good assets.
During Play, cooking can finish but asset apply waits for Stop. Unsaved source
drafts stay separate from cooked preview data. Lua controls remain in
particles_script, including `particle_system_wind(entity,x,y,z)`.

Each emitter's `quality` object has importance (0 to 1), nearDistance,
farDistance and minSpawnScale (0 to 1). Birth scale multiplies importance and a
linear distance fade between those distances. The scene bridge samples the latest
renderer primary-camera position for the next runtime tick, without a camera
only importance applies.
Quality reduces new work, existing sprites and offscreen ribbon history continue
aging and drifting. Resuming trail emission starts a separate link.

Runtime admission reserves at most 64 emitters, 65,536 GPU particles and 65,536
ribbon points, with a default 64 MiB CPU reservation limit. Instance capacity is
also bounded at construction. A rejected start preserves existing effects.
Per-frame birth work defaults to 4,096, excess births are counted and discarded
without catch-up. Session limits can be lowered from the panel when no effects
are alive, games can configure the same ParticleBudgets API. CPU bytes are a
conservative retained-data reservation including definitions, payloads and bounded
history/birth buffers, not an allocator-wide measurement.

See [acceptance results](../particle-acceptance.md) for quality settings,
measurements and remaining gates.

## Changed

*2026-10-30* � **Version 2 particle sources are gone.** Was: system source version 2
listed unique `emitters`, emitter source version 2 kept its authored order, allowed
duplicates and let a later behavior override an earlier one, and the editor could
upgrade either. Now: system and emitter sources are version 3 only, behaviors run in
registry order, each appears once, and older sources or cooked blobs are rejected
with a message to recreate them. `ParticleCheck`, `ParticleFlight`,
`FlightEffectsCheck` and the `FlightEffectsCheck` tool were deleted with them. Because:
the ordered stack and the upgrade paths doubled every authoring rule, and the only
version 2 assets were those test games.

*2026-10-30* � **The particle panel is a narrow column.** Was: a 1050 px window with a
420 px emitter column, creation beside selection and the emitter list scrolling away
with the settings. Now: a 380 px window with the system select and create bar, the edit
bar, the preview strip and the emitter list pinned above scrolling settings, and one
Save for the system and emitter. Because: the preview was hard to see and emitters were
hard to find, add and remove.

*2026-10-29* — **CPU ribbon stacks are editable.** Was: the validator accepted one fixed
shape, `RibbonSpawn` then `RibbonAge`, `RibbonWind`, a width, an optional gradient and
`RibbonFade`, and the runtime read slots by position, so the editor offered two
checkboxes. Now: any ordered list containing `RibbonSpawn` and `RibbonAge` is valid,
duplicates are allowed, later behaviors override earlier ones, and the editor uses the
same stack UI as GPU emitters. Existing assets are unchanged. Because: stack-editable
emitters in the editor.

*2026-10-29* — **Modules became behaviors.** Was: emitter sources used version 1 with
`spawnModules` and `updateModules`, and the C++ registry was `ParticleModuleInfo`,
`ParticleModules()` and `kModules`. Now: emitter source version 2 uses `spawnBehaviors`
and `updateBehaviors`, the cooked emitter content version is 6, and the registry is
`ParticleBehaviorInfo`, `ParticleBehaviors()` and `kBehaviors`. Existing assets need
the key rename and a recook, and cooked hashes change because the keys sit inside the
blobs. Because: the editor shows each entry as a behavior in an ordered stack.

The stack model follows Unreal Niagara. Each stage is an ordered list, the same
behavior can appear more than once, and a later entry overrides an earlier one that
writes the same attribute.
