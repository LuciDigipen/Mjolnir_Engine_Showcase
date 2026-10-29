# FlightEffectsCheck

Eight scripted aircraft proxies use two GPU exhaust and two CPU contrail emitters
each. Play starts the route, Stop/Play resets it. Project > Particles edits the
system and emitter sources through normal cooking. ParticleSystem user overrides
provide wind/scale controls, the script applies an initial wind and one gust at
40 seconds through particles_script.

The shared Lua route accelerates for 15 seconds, then holds speed through turns.
At 60 seconds the camera moves 10 km away, returning at 70 seconds. Emission stops
at 80 seconds. Existing 45-second trails drift/fade and retire by 126 seconds.
The route keeps flying after retirement, it does not automatically restart.
This acceptance quality tier samples contrails every four metres or 0.25 seconds.
The 96 m turning radius gives at most about 2.1 cm of chord deviation at that
spacing. ParticleFlight retains its two-metre sampling for comparison.

For a bounded game smoke without a visible window:

```powershell
.\builds\windows-msvc-debug\MjolnirEngine.exe --game games/FlightEffectsCheck --no-editor --non-interactive --hidden --frames 240
```

`--hidden` requires a bounded `--frames` run and suppresses both splash and window.
It still creates a Vulkan surface and requires a GPU. Normal launches stay visible.

## Acceptance and profiling

Build the test-only FlightEffectsCheck target with BUILD_TESTING=ON. Its hidden
window does not take keyboard/mouse focus. The tool loads this game's cooked Lua
route and particle assets, executes normal ParticleWorld controls, and submits
through the production scene bridge and GPU module. It renders cube aircraft
proxies with constant lighting, the profiling background omits the game's ground
and landmarks so the report isolates the effect workload rather than whole-game
scene cost.

```powershell
FlightEffectsCheck report.json raster on 1 0 profile timing
FlightEffectsCheck stress.json raster on 8 1000000 profile timing
FlightEffectsCheck baseline.json raster off 1 0 profile timing
FlightEffectsCheck validation.json raster on 8 0 smoke validation
```

Arguments are output, backend (`cpu`, `raster`, `rt`, `dlss`), effects (`on`, `off`),
aircraft (`1`, `8`), origin in metres (`0`, `1000000`), route (`smoke`, `profile`),
and instrumentation (`validation`, `timing`). Unsupported RT/RR returns 77 and
writes `status: unsupported`, failed checks return 1 and retain a diagnostic.

Smoke runs 240 fixed steps. Profile runs 9,000 steps, 150 simulated seconds, with
600 warmup steps. It checks old point identity, wind drift and fade across the
offscreen interval, view culling, shutdown, reservation release and complete
retirement. CPU-only profiling exercises the same script and lifecycle without
creating a window/device. The CTest case uses large coordinates in double-world
builds and the origin in float-world builds.

JSON reports contain median/p95 milliseconds by route phase, admitted/dropped work,
memory high-water marks, a ten-second lifecycle timeline and simulated/wall-clock
durations. Five captures accompany GPU profiles. Capture frames and the next
five frames are excluded from timing. GPU queries are asynchronous and lag submission
by two frame-slot cycles.
Validation and timing are separate runs, driver validation affects CPU costs.

CPU management sums synchronization/runtime, immutable submission and FX command
recording. GPU FX sums simulation, texture copies, view preparation/sorting and
HDR composition/transparent rendering, including output transitions. Mapped birth
and ribbon uploads contribute CPU submission/recording and upload-byte counters.
`frameWallMs` includes pacing/presentation and GPU waits, it is not isolated CPU
time. Compare identical on/off routes for frame-wall deltas.

Accounted memory includes CPU reservations, requested particle GPU buffers/images,
actual compositor VMA image allocations and three immutable snapshot copies. It
excludes driver pipeline/cache memory and allocator block slack, driver usage is
reported separately and can include unrelated applications. Sort scratch grows
per completed frame slot as views are used, up to 16, it stays reserved until
shutdown. Retiring effects releases history/instance reservations, reusable GPU
pools and composition images remain allocated until their owners shut down.

The one-aircraft active-phase report evaluates the working M1 targets of 0.5 ms
CPU, 2 ms GPU and 64 MiB accounted memory at 1920x1080. Budget booleans are separate
from correctness status, a passing smoke does not establish performance signoff.
See [measured acceptance](../../docs/particle-acceptance.md) for recorded results
and remaining rendering limits.
