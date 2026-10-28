# Particle composition experiment (T0)

This is a static renderer experiment, not a particle simulator. It establishes
the first integration contract for GPU exhaust and persistent contrails. The
contracts and verification procedure are recorded below.

## Run the fixture

Build `windows-msvc-debug` using the configured MSVC environment, then run:

```powershell
$env:VK_LAYER_VALIDATE_SYNC = '1'
builds/windows-msvc-debug/ParticleCompositionCheck.exe raster builds/windows-msvc-debug/probe-raster.png
builds/windows-msvc-debug/ParticleCompositionCheck.exe rt builds/windows-msvc-debug/probe-rt.png
builds/windows-msvc-debug/ParticleCompositionCheck.exe dlss builds/windows-msvc-debug/probe-rr.png cut
```

The tool opens a 960x540 window, runs 90 frames with validation, captures the last
frame and exits. Optional scenarios are `cut` (camera translation and history
reset at frame 45), `offset` (translate the scene/probe by 1,000 km), `effects`
(opaque DoF/motion blur plus bloom), and `off` (reference without the probe).
Exit 77 means the requested RT/RR mode was unavailable; it is not a passed test.
The tool never writes project settings or scene files. The CTest smoke uses raster.

`VK_LAYER_VALIDATE_SYNC` enables synchronization validation in the current SDK;
see the [validation-layer migration guide](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/main/docs/updating_from_VK_EXT_validation_features.md).
Each capture has an isolated `.png.log` engine log in addition to process output.

Expected scene: a red translucent quad and a thin green trail on two fixed world
XY planes, a blue cube in front hiding them, a white cube behind visible through
the quad, and sky visible through both shapes. Their overlap is composed back to
front. The full-screen fragment experiment analytically intersects those planes;
it does not allocate particles, generate trail history or prove GPU simulation.

## Selected rendering contract

Raster: scene/sky -> opaque DoF/motion blur -> probe -> bloom -> tonemap/FXAA.
RT: trace -> RR when active -> opaque DoF/motion blur -> probe -> bloom -> tonemap.
The pass is inactive by default and allocates images only on its first enabled
frame. It creates one independent RGBA16F attachment per frame slot, samples the
upstream HDR color/depth and writes the composed color. It never aliases its
sampled source with the attachment or writes opaque depth. Existing post effects
retain their lifecycle and ordering when the probe is disabled.

Raster depth is sampled after PostProcessingStack's existing transition to
SHADER_READ_ONLY. Inverse projection reconstructs an actual point and ray length,
including off-axis rays and finite/infinite far projections. RT's distance buffer
already stores ray length, so it must not be compared directly to raster depth
or view-Z. RT depth lookup compensates pixel jitter with nearest sampling; this
remains an approximation at silhouettes. Probes use the unjittered resolved view
and spatial edge coverage, do not enter RR guides/history, and are not blurred by
DoF/motion blur. Those are explicit M1 limits, not final RT acceptance.

ParticleProbeView is a 128-byte CPU/Slang push layout: inverse projection (64),
camera-relative world anchor (16), pixel viewport (16), inverse image extent and
depth mode (16), and depth jitter (16). World anchor minus camera is computed in
WorldReal before narrowing. Each raster view receives its own conversion; RT
retains the engine's current single-view restriction. Singular/invalid inputs
are skipped. The output is initialized outside view rectangles too.

The probe proves inter-effect ordering only. Existing transparent meshes are
already in upstream color, so exact glass/water interleaving is not established.
M1 uses bounded page/group ordering plus per-particle sorting where required and
per-segment trail ordering. General OIT stays deferred. Any visible failure with
intersecting translucent geometry must be documented at T5.

## Contracts frozen for T1-T3

These are design contracts for later implementation, not allocated runtime
buffers or promises that T0 has implemented a simulator.

| Item | Contract |
|---|---|
| Ownership | particles owns logical instances and CPU history; renderer owns GPU buffers/passes; particles_runtime converts immutable packets; particles_script owns Lua bindings. No renderer -> particles dependency. |
| Identity | CPU instance `{index, generation}`; GPU identity includes instance generation, page index and stable spawn ID. Stale references never silently resolve to reused storage. |
| Frame packet | Monotonic simulation tick, accepted fixed steps, immutable spawn batches, page anchor updates, retired handles and trail sample ranges. Only copies/counts needed by the GPU enter staging. WorldPosition anchors stay CPU-side. |
| GPU particle layout | Proposed 80-byte record: local position/age (16), world-axis velocity/lifetime (16), size/rotation (16), linear RGBA (16), instance/page/spawn/material IDs (16). Explicit CPU/Slang offset checks are required at T2. |
| Page | Stable translation-only WorldPosition anchor; 1,024 m component-wise local radius. At that scale float spacing is about 0.12 mm, below the 1 cm fixture tolerance. New emission outside the radius uses a new page; old particles stay anchored. Particles that leave the supported local radius must migrate or retire under a counted policy. |
| World queries | Keep anchor, local position, identity, material/opacity and trail segment endpoints independently of billboard vertices. T8 can evaluate ray-visible geometry/procedural proxies without recovering world data from a camera-facing quad. |
| Ordering | Input producers write transforms before Propagate. Particle collection runs in LateUpdate at order 70; bridge submission at 80; Render consumes once. Producers needing same-phase particle inputs run earlier than 70. Equal orders do not express dependencies. |
| Timestep | Fixed 1/60 s effect steps, fractional remainder retained, max four steps per rendered frame; drop excess catch-up work with a counter. Spawn interpolation uses accepted swept motion. GPU and CPU trail clocks share accepted simulation time. |
| Trail sampling | Baseline spacing 10 m with a 0.1 s maximum sample interval, 120 s visual lifetime and 1 cm coordinate-equivalence tolerance. Explicit teleport/activation breaks never interpolate across the gap. Limits can reduce admitted history under the global sample cap. |
| Skipped frames | Freeze effect time and discard pending emission when rendering cannot submit (e.g. minimized). On resume, reset swept-pose history and start at the current attachment; do not emit along the unobserved gap or replay unbounded requests. |
| Lifecycle | Stop drains; destroy/reset kills and invalidates the instance. Owner removal drains under the existing bounds with a copied final pose; scene unload kills all effects and retires GPU references on their last timeline value. Pause freezes time; PlayBegin/PlayEnd reset; asset reload resets affected instances after commit, and abort preserves active assets. Preview advances only when explicitly enabled. |
| Synchronization | One graphics+compute+present family, explicitly validated at T2. Serialize persistent compute writes and render reads across frames with synchronization2; slot-owned uploads/descriptors/stats cannot be reused until the frame timeline completes. No new steady-state idle/readback waits. |

## Workload and budgets

Baseline is one aircraft with two exhaust and two trail sources, 1080p at 60 Hz.
Proposed M1 caps: 64 instances, 256 pages, 65,536 live GPU particles, 16,384 trail
samples, 4,096 new particles/frame, and 1 MiB upload/frame. Excess emission is
rejected/counted; draining pages/samples retire by age before lower-priority
history is evicted. Offscreen trails age/drift; draw culling never resets history.
An allocation request cannot exceed the total budget even when another limit
allows it.

The provisional budget remains p95 0.5 ms CPU and 2 ms GPU for all effect work,
with 64 MiB total effect-owned CPU+GPU memory. At 1080p the two RGBA16F outputs
alone consume about 31.6 MiB; double-buffered 80-byte particles add 10 MiB before
indices, trail history, staging and scratch. Account for all these resources;
unlimited per-effect intermediates would violate the budget. These are targets,
not measured particle-system performance. The 960x540 fixture is correctness
evidence, not a 1080p benchmark. Stress workload is eight aircraft under the same
hard limits with explicit quality degradation.

Current shader compilation uses `-g -O0` even with a Release C++ build. Record
those flags with measurements; shader optimization is not changed by this task.
Capture reference CPU/GPU/driver, resolution, AA mode and available RT/RR support
with each verification lane. No M1 performance target includes future full RT
participation without a separate measured budget.

## Final ray-tracing scope

T8 will choose a measured world-space representation for ray-visible reflections,
including offscreen effects. T9 adds shadow transmittance and a stated self-shadow
approximation. T10 connects indirect lighting received by particles, opacity's
effect on transport, and emission into the engine's GI path, with reconstruction
history/guide handling. These remain required final features; the T0 overlay is
not their implementation or proof. Raster/hybrid fallbacks remain available.

## Verification boundaries

Focused CPU tests cover near/far-origin equivalence, inverse projection including
off-axis rays, and invalid inputs. On-device captures must separately establish
raster occlusion, RT occlusion, RR on/off and cuts. Run the existing smoke suite
with the probe disabled, and the added fixture with validation/synchronization
validation. Record unavailable or unrun modes as pending in the implementation
plan. No emitter asset/module, compute particle simulation, authoring graph,
physical weather or fluid model is introduced by T0.

## Verification recorded 2026-10-27

Reference machine: Intel Core i9-13900H (14 cores/20 logical processors), NVIDIA
RTX 4060 Laptop GPU, Windows driver `32.0.16.1062`, reported Vulkan `1.4.341`.
Build: VS 2026 / MSVC 14.51.36231, Debug C++, Vulkan SDK 1.4.357.0, Slang `-g -O0`.
Captures are 960x540. These are correctness checks, not a particle benchmark.

| Check | Result |
|---|---|
| Debug engine, fixture and renderer tests | Built with DLSS disabled, then with the existing DLSS option enabled. |
| Renderer suite | 103 test cases, 54,012 assertions passed, including three new probe cases. |
| Regression and fixture smokes | 8/8 passed, including EmptyScene, HelloWorld, TerrainCheck, UI, game mode, project switching and particle composition. |
| On-device matrix | 10/10 exit 0: raster still/off/cut/offset/effects, RT still/off/cut, RR still/cut. |
| RR actually executed | NGX logs confirm successful evaluation and history reset, including the cut at frame 45. |
| Pixel comparisons | Foreground and untouched sky match probe-off references; quad/trail change sky and rear geometry. Raster/RT/RR agree within 3/255 at four interior samples. |
| Validation/lifetime | Synchronization validation enabled; no VUID/SYNC errors in the ten isolated logs; all ten report zero VMA live allocations at shutdown. |

Evidence is under `builds/windows-msvc-debug/particle-t0/`: `matrix.json`,
`renderer-tests.log`, `smokes.log`, `pixel-checks.json`,
`pipeline-pixel-comparison.json`, and each case's PNG/process/engine logs.
Earlier `raster.png`/`rt.png` were preliminary captures; use the final
`raster-still.png`, `rt-still.png` and `dlss-*.png` captures instead.

The ordinary SDK clone stalled before data transfer. Verification used a sparse
checkout of the same pinned SDK tag `v310.9.16`, commit
`9981c6350dc58c0ce21f024fd65e31e851c31689`, selecting `include`,
`lib/Windows_x86_64/x64` and `lib/Windows_x86_64/rel`. The local CMake cache uses
`MIR_WITH_DLSS=ON` and `FETCHCONTENT_SOURCE_DIR_NGX_SDK` pointing to
`builds/windows-msvc-debug/ngx-sdk-t0`. No SDK source or default build option was
changed in the repository. Initial sandbox-only PDB and temporary-file test
failures were resolved by using the initialized MSVC environment outside the
sandbox; no production workaround was added.

Remaining gates belong to later tasks: particle/trail simulation and its timings,
1080p budget signoff, float-world and game-only build lanes, multi-view and resize
exercises, exact transparent-mesh interleaving, temporal treatment at depth edges,
and the required final reflections/shadows/GI. The probe's analytic planes establish
composition behavior, not production particle throughput or ray-visible particles.

## Release cleanup gate

The production CPU effect definitions and lifecycle now live in the
[particles module](modules/particles.md). They do not call the temporary probe.
GPU simulation and production drawing remain separate follow-up work, so this
fixture is retained as a composition reference for those changes.

Once production particle rendering replaces this experiment, remove the temporary
probe API, pass, shader, probe-only data helpers and build registrations from the
engine. Disabling the probe at runtime does not satisfy release acceptance.
Final game binaries and packaged shaders must contain none of this experiment.
Useful regression cases may remain in test-only targets, rewritten to use the
production rendering API rather than retaining temporary production functions.
