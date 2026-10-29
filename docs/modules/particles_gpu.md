# particles_gpu

GPU particle simulation and sprite/ribbon rendering behind the renderer FX
interface. Dependencies are renderer (public) and VMA (private). The renderer owns
composition images, frame scheduling and image transitions.

## Ownership and frame flow

`ParticlesGpuModule` registers one `IFxSystem`. `particles_runtime` creates immutable
GPU-emitter and CPU-ribbon snapshots. Simulation clocks, births and ribbon history
remain in particles, the bridge owns no persistent state.

`RecordSimulation` dispatches simulation once after the existing frame-slot wait.
`PrepareDraw` runs outside dynamic rendering and prepares independent view-local
sort/cull results. `RecordDraw` executes inside the renderer's HDR composition pass.
Additional cameras never dispatch simulation again. Camera cuts and backend changes
rebuild view data while preserving particles. Scene/play/reload clears discard all
pending snapshots and render work. Replaced frame snapshots discard unconsumed
births instead of accumulating a catch-up queue while rendering is skipped.

## Simulation resources

Each GPU emitter owns a first-fit range in a 65,536-record pool, with at most 64
ranges and 4,096 births per frame. Generation or capacity replacement resets only
that range. Cooked scripts use the 160-byte input and 16-byte birth ABI, compact
survivors and generate counts on the graphics+compute queue. No runtime source
compilation, synchronous particle readback or steady-state idle wait is used.

All emitters in a system share a world-precision tile origin. Old/new tile deltas
are computed before narrowing, applied inside normal simulation, and independent
of view count. GPU survivor positions have a 65,536 m local precision budget.
Float-world submissions reject unsupported absolute coordinates.

Unused script pipelines and texture uploads retire through their final timeline
use. Shared sprite/ribbon textures are keyed by asset identity and pixel content.
RGBA8 images are bounded to 2,048 pixels per side and a 64 MiB resident/upload
budget. Linear sampling clamps atlas coordinates inside half-texel cell borders.
Ribbon U coordinates repeat, V coordinates clamp.

## Per-view visibility and ordering

Sprites use GPU sphere/frustum and distance culling followed by deterministic
bitonic sorting of indices, leaving the simulation pool unchanged. Non-power-of-two
capacities pad to the next power of two inside twice the reserved range. Alpha
particles sort farthest first, ties use pool index. Sort none preserves pool order
while compacting visible indices. Conservative bounds retain particles crossing a
frustum or distance boundary.

Sixteen views match the renderer limit. Two frame slots reserve 32 MiB of sort
scratch, plus bounded indirect arguments and asynchronous diagnostic readback.
Worst-case sorting is O(C log-squared C) for padded capacity C per view, acceptance
profiling and quality policies remain later work. `ParticleGpuStats` reports
completed visibility counts and the first sorted index for each view.

CPU ribbon geometry subtracts each view's camera in world precision before
narrowing. Shared strip corners join adjacent links, missing links and ribbon IDs
never connect. Duplicate samples emit no triangles, end-on views use finite
fallbacks. Segments use conservative view bounds and the selected sort order.
Whole-repeat subtraction preserves texture phase after prefix eviction.

Two mapped buffers reserve about 6 MiB for at most 65,532 ribbon vertices across
all views per frame. Exhaustion truncates geometry while CPU history keeps aging
and drifting. Samples beyond 10,000 km on any camera-relative axis omit geometry.
View/material records add about 1.3 MiB across both slots. Host writes occur only
after that slot's timeline wait, sort writes are explicitly exposed to indirect,
vertex and diagnostic transfer reads.

Sprite emitters and ribbon batches sort together by their CPU-known group proxy,
emission position for sprites and mean visible vertex distance for ribbons.
Sorting within a sprite emitter or ribbon is finer than this group order.
Intersecting groups can still blend incorrectly, there is no global per-element
interleaving or general OIT.

## Shading and composition

Cooked renderer properties select sprite screen/velocity alignment, camera-plane
or camera-position facing, alpha/additive blend, sort mode, texture use, soft depth
distance, sun/ambient lighting, fog and maximum draw distance. Ribbon alignment
follows the trail tangent, facing selects plane/position behavior. Properties can
change while existing particles remain alive.

The shared material path samples opaque depth using each view rectangle and RT
jitter. Raster reversed-Z depth reconstructs camera-relative position before a
metric soft-gap comparison, RT uses ray distance directly. Fragment position is
interpolated before measuring distance, avoiding corner-distance interpolation
errors. A zero soft distance selects a hard depth cut.

Lit materials use two-sided sun diffuse plus the renderer's ambient color, without
self-shadowing or volumetric scattering. Scene and FX share the distance-fog
transmittance helper. Alpha materials approach fog color, additive emission
attenuates through fog. Derivative-based edge coverage provides spatial smoothing.

Raster order is opaque geometry, sky, blended meshes, opaque DoF/motion blur, FX,
bloom, tonemap and optional FXAA. RT order is trace, RR where enabled, opaque
DoF/motion blur, FX, bloom and tonemap. Disabled bloom contributes zero and its
uninitialized output is not sampled. FX composition samples depth without writing
it and keeps the existing single-view RT limit.

Particles receive bloom/tonemap and raster FXAA where requested. They do not
receive opaque DoF/motion blur, ray-traced MSAA or RR temporal reconstruction and
do not contribute RR guides or transparent velocity. Existing glass/water draws
precede the entire FX layer, so front/intersecting glass may incorrectly appear
behind particles. RT blended-mesh behavior remains the existing renderer's behavior.
Particles are not yet visible to secondary reflection, shadow or GI rays.

## Verification

ParticleGpuBudgets configures immutable admission/upload limits. Defaults are
64 MiB of copied submission data, 64 MiB of resident and staging texture data,
16 MiB of new texture uploads per frame, 4,096 uploaded births and 65,532 ribbon
vertices. Values can be lowered within the compiled ceilings. Snapshot overflow
rejects whole emitter rows, birth overflow drops excess births, ribbon overflow
truncates segments. These paths expose counters. Texture uploads that exceed
either texture cap defer the image and draw with the shared white fallback,
retrying on later frames. Retired textures remain accounted until their timeline
completion, so deferred images cannot overrun memory while old work is in flight.

Diagnostics expose completed GPU alive counts, tracked buffer/texture bytes and
high-water marks, copied snapshot bytes, upload bytes and rejected/dropped work.
Tracked GPU bytes count requested owned buffers, images and staging data, not
driver allocations, pipelines or total process VRAM. CPU timings cover simulation,
submission and command recording. Three timestamp scopes measure simulation plus
texture uploads, view preparation/sorting, and drawing. Completed frame-slot
queries are read with availability flags, without WAIT, queue idle or synchronous
particle readback. Unsupported queues report timing unavailable. Values can lag
the displayed simulation, and are diagnostics rather than T7 performance signoff.

`ParticleGpuCheck <output.png> [raster|rt|dlss]` cooks its inputs through AssetCooker
and exercises the production API with synchronization validation. It verifies
opposite orders for two raster cameras, conservative culling, no extra simulation
or birth replay, scene clear, registration replacement, resize and backend switches.
Named captures cover a particle-free sky/opaque baseline, hard and soft depth,
textured overlapping smoke/trails, blended mesh limits, sun/ambient, fog, additive
facing, split-view depth and opaque post effects. Headless module tests cover
property validation/cooking and geometry bounds, links, joins and remote cameras.
