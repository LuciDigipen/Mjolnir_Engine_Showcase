# Particle acceptance, 2026-10-29

T7's scripted flight scene and measurement fixture are implemented. Correctness
checks pass, but performance signoff remains open against the M1 limits below.
T8 through T11 remain separate work, including reflection, shadow and GI rays.

## Route and method

`games/FlightEffectsCheck` uses one or eight aircraft, each with two GPU exhaust
and two CPU contrail emitters. The shared Lua route accelerates, turns, changes wind,
moves the camera 10 km away and back, stops emission at 80 simulated seconds and
continues to 150 seconds. Contrails use a four-metre or 0.25-second sampling tier,
with 45-second lifetime. Eight-aircraft runs start one million metres from origin.

Release profiles render at 1920 x 1080 through raster, RT and DLSS/RR paths, with
effects on and off. They run 9,000 fixed 60 Hz simulation steps, discard 600 warmup
steps, and exclude captures and five following frames from timing. Validation runs
are separate. GPU timestamps are read after completed frame-slot reuse. CPU
management includes runtime synchronization, snapshot submission and FX command
recording. GPU FX includes simulation, texture copies, sort preparation and HDR
composition. Frame wall time includes pacing and presentation.

Reference system: RTX 4060 Laptop GPU, Intel i9-13900H, Windows 11, Vulkan 1.4.341,
driver 610.62. Release C++ uses `/O2 /Ob2 /DNDEBUG`, cooked Slang shaders retain
the project's `-g -O0` setting. These are single-machine observations, not a
cross-device guarantee.

| One aircraft, active phase | CPU median / p95 ms | GPU median / p95 ms | Accounted high-water MiB |
| --- | ---: | ---: | ---: |
| Raster | 0.328 / 0.531 | 1.170 / 1.244 | 49.23 |
| RT | 0.792 / 3.665 | 1.290 / 2.005 | 49.23 |
| DLSS/RR | 0.395 / 0.747 | 1.357 / 1.823 | 49.23 |

The M1 limits are CPU p95 at most 0.5 ms, GPU p95 at most 2 ms, and accounted
effect memory at most 64 MiB. Raster misses CPU by 0.031 ms, RT misses both limits,
and DLSS/RR misses CPU. Each report has phase medians/p95, on/off wall times, work
counters, memory breakdown, a ten-second lifecycle timeline and five PNG captures.
Reports and captures are local in `builds/particle-t7-verified-*`.

The memory sum includes CPU reservations, requested particle GPU resources, actual
composition image allocations and three immutable snapshot copies. It excludes
driver pipelines, cache allocations and allocator block slack. Driver usage is
reported separately. The route verified wind drift and fading of an identified
old trail point, offscreen history, GPU culling and return, zero instances/history
and live GPU particles after drain, and CPU reservation release. No admission,
birth upload, ribbon geometry or texture deferral counters increased in the
normal route.

Focused Debug, float-world and game-only particle suites passed. Nine hidden
game/GPU smokes passed in both Debug and game-only builds. Raster, RT and DLSS/RR
GPU fixtures passed synchronization validation. A production Release build with
`BUILD_TESTING=OFF` exits successfully; the game binary and packaged shaders have
no particle probe markers, and no probe shader is packaged. The source/header
audit found no new file header on a non-`.h` or non-`.cpp` file.

Secondary reflection, shadow and GI ray participation are not implemented yet.
See T8 through T11 in the local particle plan for those acceptance gates.
