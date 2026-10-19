# Terrain streaming measurements and selected budgets (#130)

This report records the performance evidence behind TerrainCheck's streaming settings.
Measurement instrumentation, benchmark drivers, fault injection, aggregation scripts and
raw traces are retained by the terrain maintainer locally. They are not engine features
or repository test targets. The measurements include the cost of the local instrumentation.

## Method and selected configuration

The tuning runs used a fixed 1/60 s simulation tick and repeated fresh engine processes.
The route sequence contained high altitude (240 frames), approach/landing (600), straight
travel at 200 m/s (1080), sharp turns at 180 degrees/s (360), four teleports (120 each),
two viewports with three collision interests (300), and reload with two recoverable
payload-read failures (300): 3,360 frames per raster or RT run. All timing percentiles
use nearest rank; unsuccessful/canceled and outstanding requests are separate from
successful readiness samples. Queryable means decoded heights installed; renderable
means the GPU mesh is usable; collidable means active colliders are placed.

TerrainCheck's selected settings are:

| Area | Selected values |
|---|---|
| CPU residency | 32 MiB; Interest mode; 2 workers; 32 installs/frame; 2 s query lease |
| View selection | refine 1 px; merge 0.75 px; prefetch 0.5 px with 12 MiB budget |
| GPU meshes | 128 MiB; 4 MiB/frame upload; 16 MiB staging; 4 s retention |
| Collision pieces | 2 tiles; 64 m seam overlap; 8/4/2/1 m spacing candidates |
| Collision accuracy | 0.05 m maximum error and body tolerance |
| Collision prediction | 0.1 s initial load estimate + 0.25 s margin; 1 s prefetch |
| Collision residency | 16 MiB shape budget; 4 s retention |

These caps support the measured fixtures and release overlap. They are not general
requirements for larger maps, and the measurements do not establish a performance gate.

## Tuning measurements

**Target-machine measurements.** MSVC Release, 1280 x 720, validation off,
Ryzen 7 5800H, nominal 16 GiB RAM, RTX 3070 Laptop (8 GiB VRAM), NVMe.
The two committed maps are a mixed-resolution 2 x 2 map and a 16 x 16 streamed
map. Six controls ran raster and RT twice (80,640 frames); the combined
recommendation ran both twice again (13,440 frames). Tables show ranges
across repetitions, not percentiles pooled across distinct runs. Each run
used a fresh engine residency; Windows file cache was not flushed.

| Case | CPU / mode | Prefetch | GPU / upload / staging (MiB) | Sharp-turn missing frames | Straight / multi-view missing frames |
|---|---|---:|---|---:|---|
| Original control | 16 / Auto | 4 | 128 / 8 / 32 | 92 | 6 / 11 |
| More prefetch and retention | 16 / Auto | 8 | 128 / 8 / 32 | 32 | 0 / 0 |
| Larger caches and Interest | 32 / Interest | 12 | 192 / 8 / 32 | 0 | 0 / 0 |
| Tight upload | 16 / Auto | 8 | 64 / 2 / 16 | 131 | 0 / 3 |
| Relaxed LOD thresholds | 16 / Auto | 8 | 128 / 8 / 32 | 4 | 0 / 0 |
| Smaller collision pieces | 16 / Auto | 8 | 128 / 8 / 32 | 32 | 0 / 0 |
| Recommendation | 32 / Interest | 12 | 128 / 4 / 16 | 0 | 0 / 0 |

All controls and recommendations had zero duplicated tile coverage and zero
main-thread payload range reads. The source wrapper does not intercept the
existing synchronous small `current` pointer reads during Edit polling;
their cost remains included in CPU frame time but is not a separate IO sample.
Zero range reads therefore does not mean zero main-thread filesystem calls.
The larger-cache control changes policy as
well as memory, so it does not isolate the CPU-cap effect. The recommendation
keeps the original LOD accuracy thresholds, raises prefetch and retention,
and halves upload/staging capacity. Sampled peaks were 14.66 MiB CPU payload,
41.17 MiB raster or 80.73 MiB RT meshes plus staging capacity, and 0.463 MiB
collision. The caps allow handover and release overlap; they are not measured
requirements for a larger map. Process working set was 272.88–304.35 MiB
(including the trace); sampled device allocation was 384.74 / 432.11 MiB.

| Successful readiness (ms) | Raster P50 / P95 / P99 | RT P50 / P95 / P99 | Samples per run |
|---|---|---|---:|
| queryable | 16.10–16.41 / 17.37–17.83 / 266.82–283.54 | 16.51–16.56 / 17.40–18.43 / 230.21–282.63 | 125 |
| renderable | 205.98–206.84 / 392.45–393.37 / 426.54–426.78 | 273.85–310.66 / 694.57–750.55 / 743.61–800.44 | 544 / 541–542 |
| collidable | 48.81–49.53 / 199.48–199.75 / 199.48–199.75 | 49.79–50.46 / 198.82–260.11 / 198.82–260.11 | 11 |

Query tails include the two injected read failures and retries. Each run
recorded both failed range/checksum attempts separately; RT runs canceled
two or three renderable requests at the scene reload, and all runs ended
with no pending requests. With eleven collision samples P95/P99 equal the
maximum; that is a small sample, not a latency guarantee. Height-query P99
was 0.006–0.007 ms, mesh-build P99 0.356–0.400 ms, and upload-to-ready P99
33.13–34.21 ms. Full stage distributions remain in the JSON.

| Route | CPU frame P99, raster / RT (ms) | GPU frame P99, raster / RT (ms) | Missing frames per run |
|---|---|---|---:|
| High altitude (240 frames) | 5.81–6.12 / 3.87–4.22 | 1.74–2.09 / 2.18–2.28 | 31–33 initially |
| Approach and landing (600) | 3.84–3.89 / 3.64–3.95 | 4.76–4.76 / 5.40–5.64 | 0 |
| Straight, 200 m/s (1080) | 3.21–3.41 / 3.69–3.70 | 5.56–5.60 / 5.23–5.56 | 0 |
| Sharp turns, 180 degrees/s (360) | 3.24–3.46 / 3.33–3.41 | 2.97–2.97 / 4.89–5.02 | 0 |
| Four teleports (120 each) | 3.07–4.19 / 3.04–8.54 | 2.74–3.04 / 3.31–15.84 | 2 / 2 / 0 / 2 |
| Two views, three interests (300) | 3.52–3.73 / 3.47–8.27 | 2.30–2.31 / 2.30–2.82 | 0 |
| Reload with two read failures (300) | 6.42–6.65 / 4.25–6.54 | 5.80–6.02 / 6.41–7.14 | 16–20 initially |

The RT multi-view route uses the existing raster fallback. Raster draw-call
P95 is 8 at high altitude, 133 on approach, 175 on the straight route and
83 on turns; actual primary RT draw calls are zero. Every steady movement
route had zero device/queue idle waits. Startup CPU frames remained
349–370 ms in the final runs (the controls reached 570 ms); reload frames
were 40–53 ms. These spikes and initial holes remain in the report. The
reporting threshold is not a performance gate, and these fixtures do not
establish a 150 km or cold-disk guarantee. A control repetition overlapped a
local read-only audit and showed extra CPU spikes; raw repetitions were
retained rather than discarded. The recommendation was measured without
build/test interference. Per-route telemetry P99 reached 0.22–1.24 ms and
is included in CPU frame times.

The existing Region experiment was rerun: 4096 m / 128 cells produced
17,613,912 bytes in 10.80 s, with an L0 bound around 5.8 m, versus
8192 m / 128 cells at 4,329,392 bytes in 7.47 s and around 15 m. Keep
4096 / 128 for the finer hierarchy. A local mesh comparison (50 repeats)
found native build / decoded-cache assembly means of 6.158 / 23.217 ms at
1 m and 0.104 / 0.327 ms at 8 m. Cache assembly includes provenance
validation; the native fixture has heights and neighbors already resident.
Retain the existing LOD/Normal format for mixed-resolution seams, without
claiming a CPU win or introducing a full renderer-vertex disk cache.

Two additional 1440-frame RT routes published during Play, stopped, prewarmed
and adopted the new release, then played it. The query release remained A
during publishing and became B only after Stop; the source-limit height at
the fixed probe changed from -2.7413 to 3.0488 m. A second near-ground
interest actually built a staged B collider: 0.319 MiB held beside A for
three sampled frames. Active plus staged release shares peaked at 15.70 MiB
CPU, 36.04 MiB GPU and 0.657 MiB collision (including the unchanged second
asset); total meshes/retirement/staging peaked at 81.98 MiB. Verification
took 44.92–47.11 ms and preparation 278.50–278.77 ms, one sample per route.
Publishing and Play B had no missing frames; Stop's scene-resource reset
had three. Duplicate coverage and main-thread ranges remained zero. The
near-ground Stop route had CPU/GPU P99 4.08/5.60 ms; Play entry retained a
28.12 ms CPU spike. First-mesh admission may exceed the upload frame budget,
as in #127: the switch observed 13.94 MiB frame cost including BLAS, with
10.60 MiB staging used. The 4 MiB setting is not a strict per-mesh limit.
An attempted repeat with an already published candidate correctly failed
without changing current; its incomplete trace is kept separately. Repeating
the publish route requires baking a new candidate identity.

## Validation after pipeline integration

The final recommended configuration was replayed once in raster and once in RT after
integration with the terrain pipeline. Each run completed 3,360 frames and retained
3,358 GPU timing samples, with zero duplicated tile coverage and zero main-thread
payload-range reads. These were integration checks, not a repeat of the full tuning
matrix above. The local native/cached mesh comparison also passed 100 samples.

The repository keeps ordinary terrain, renderer, physics, script and engine smoke
coverage. Benchmark-specific tests and runtime hooks are maintained locally with the
measurement tools.
