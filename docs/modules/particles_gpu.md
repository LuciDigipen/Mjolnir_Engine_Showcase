# particles_gpu

GPU particle datasets, compute simulation, sprite and ribbon rendering behind the renderer FX interface. The renderer owns composition images, frame scheduling, and synchronization.

`mir::particles_gpu` ? depends `mir::renderer` (public), VMA (private) ? API `modules/particles_gpu/include/particles_gpu/` ? maintainer Sumin Park ? reviewed 2026-10-28

## Model

`ParticlesGpuModule` registers one `IFxSystem` with `VulkanRenderer`. The module composes a dataset, compute dispatcher, and sprite renderer. `particles_runtime` builds stateless per-emitter `ParticleSubmission` records from CPU runtime state and submits the current snapshot. The bridge does not own clocks, fractional births, or persistent emitter identity.

The renderer calls `RecordSimulation` once after the frame slot wait. It calls `RecordDraw` inside its own HDR composition rendering after opaque effects and before bloom. The public FX interface is the renderer's Vulkan boundary; image ownership and transitions remain in renderer.

## API

| Type | Header | Role |
|---|---|---|
| `ParticleSubmission`, `ParticleStats` | `particles_gpu/ParticleSubmission.h` | Per-emitter input snapshot and completed counters |
| `ParticlesGpuModule` | `particles_gpu/ParticlesGpuModule.h` | Register backend, submit snapshots, inspect stats, clear scene |

## Rules

Each accepted emitter owns a first-fit range in the shared 65,536-particle record buffer. There are at most 64 active ranges and 4,096 accepted births per frame. Capacity and range exhaustion drop work. Generation or capacity replacement resets only that range.

Compute compacts survivors and writes indirect draw counts. Persistent buffers are shared between two frame slots, so same-queue barriers order range reuse and expose compute output to indirect and vertex reads. Per-slot descriptors, inputs, counter readback and completion values follow the renderer's timeline wait. Counter queries expose only completed frames.

A scene clear discards pending submissions and live ranges. Unregistration waits for prior GPU uses before module resources are destroyed. `particles_gpu` does not belong to `PostProcessingStack` and does not own HDR composition targets.

## State

As of 2026-10-28, GPU simulation and radial sprite draws work for raster and RT/RR composition. The backend has two frame slots, a 65,536-record pool, 64 emitter ranges, per-frame birth and range bounds, timeline-completed counters, and resize/unload support. Ribbons have per-segment sorting. It has no sprite sorting, lighting, or particle participation in reflections, shadows, or GI.

`ParticleGpuCheck` exercises the production module, including two distinct emitter ranges from one system. Renderer raster comparison against the T2 output is pixel-identical. The fixture also checks RT and DLSS/RR modes, scene clear and backend unregistration. RenderDoc inspection records compute dispatch, counter data, draw, and synchronization barriers.

## Backlog

Later tasks add production renderer properties, sprite sorting, lighting, and RT integration.

## Cooked dispatch

Each submission includes its cooked script identity and SPIR-V bytes. The dispatcher
caches one Vulkan compute pipeline per script and dispatches each active emitter
with its own four-byte index push constant. The input ABI is now 160 bytes, with a separate 16-byte birth record buffer.
Inactive ranges have their indirect counters cleared. The sprite shader contains
only drawing, compute shaders arrive through cooked asset dependencies.

Unused pipelines remain alive through their final submitted timeline value. The
existing frame-slot wait proves a completion lower bound, permitting retirement
without a new host wait or device-idle operation. Changed stacks use new script
identities and can coexist with prior pipelines while earlier frames finish.
The GPU regression fixture cooks its inputs through AssetCooker and exercises
repeated stack replacement alongside the original lifetime and capacity checks.

## Moving exhaust resources

Submissions contain previous/current attachment positions, a shared system tile
origin, world-relative velocity and wind, seeded birth records, and loaded RGBA8
texture data. The GPU input remains bounded by 64 emitter slots, 65,536 particles
and 4,096 births per frame. Tile deltas are computed before narrowing and applied
inside the update shader, independent of view count. The old 1,024 m emission
cutoff is gone. The 65,536 m local survivor budget is a precision limit, not a tile
boundary. Float-world submissions reject unsupported absolute origins.

The sprite renderer uploads textures in RecordSimulation after dispatch, outside
dynamic rendering. Per-emitter/per-frame descriptors bind the atlas at binding 7.
Textures are keyed by asset identity and pixel content, replacing an image retains
its prior allocation until its last submitted use completes. Staging allocations
retire through the same frame-slot completion bound. Individual images are at
most 2,048 by 2,048, with a 64 MiB resident/upload budget. A radial sprite remains
the default when no Flipbook operation is authored.

Diagnostics asynchronously copy counters and one record per emitter, using the
existing frame-slot timeline wait. First-particle world position, velocity, age,
size, alpha, frame and sequence allow numerical GPU regression checks without
synchronous state readback. Tests cover swept initialization, stationary survivors
across repeated tile changes, one-million-metre translated births, seed replay,
lifetime curves, flipbook advance and texture replacement while frames are in flight.

## CPU ribbon rendering

The scene bridge snapshots CPU history into `RibbonSubmission` without simulation
state. `BuildRibbonGeometry` subtracts each view camera in world precision before
narrowing. Shared strip corners join adjacent links, disconnected ribbon IDs and
missing links never connect. End-on views use a finite perpendicular fallback,
duplicate samples emit no triangle. Segments sort back to front per view.

The renderer uploads at most 65,532 vertices across all views in one frame,
using two mapped storage buffers (about 6 MiB total), protected by the existing
frame-slot timeline wait. Capacity exhaustion truncates geometry, simulation
continues. Views farther than 10,000 km from a sample omit it. This avoids float
geometry overflow and never changes stored history. Camera changes rebuild only
geometry. No extra queue wait, readback, or device-idle operation is introduced.

Ribbon shaders sample the existing raster reversed-Z or RT ray-distance depth,
blend premultiplied HDR into the FX target, and inherit the existing placement
after opaque DoF/motion blur and RR, before bloom/tonemapping. Width falloff and
a periodic longitudinal pattern use continuous distance-based UVs, subtracting
whole repeats preserves phase after eviction. Ribbon shading is procedural in
this step, the emitter texture remains a validated dependency. Textured renderer
properties, fog, lighting, sprite/ribbon interleaving and soft intersections belong
to the production rendering task. Ribbons do not participate in RT secondary rays.
