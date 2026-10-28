# particles_gpu

GPU particle datasets, compute simulation, and sprite rendering behind the renderer FX interface. The renderer owns composition images, frame scheduling, and synchronization.

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

As of 2026-10-28, GPU simulation and radial sprite draws work for raster and RT/RR composition. The backend has two frame slots, a 65,536-record pool, 64 emitter ranges, per-frame birth and range bounds, timeline-completed counters, and resize/unload support. It has no textured sprites, ribbon renderer, sorting, lighting, or particle participation in reflections, shadows, or GI.

`ParticleGpuCheck` exercises the production module, including two distinct emitter ranges from one system. Renderer raster comparison against the T2 output is pixel-identical. The fixture also checks RT and DLSS/RR modes, scene clear and backend unregistration. RenderDoc inspection records compute dispatch, counter data, draw, and synchronization barriers.

## Backlog

Later tasks add production renderer properties, ribbons, sorting, lighting, and RT integration.

## Cooked dispatch

Each submission includes its cooked script identity and SPIR-V bytes. The dispatcher
caches one Vulkan compute pipeline per script and dispatches each active emitter
with its own four-byte index push constant. The shared input ABI remains 80 bytes.
Inactive ranges have their indirect counters cleared. The sprite shader contains
only drawing, compute shaders arrive through cooked asset dependencies.

Unused pipelines remain alive through their final submitted timeline value. The
existing frame-slot wait proves a completion lower bound, permitting retirement
without a new host wait or device-idle operation. Changed stacks use new script
identities and can coexist with prior pipelines while earlier frames finish.
The GPU regression fixture cooks its inputs through AssetCooker and exercises
repeated stack replacement alongside the original lifetime and capacity checks.
