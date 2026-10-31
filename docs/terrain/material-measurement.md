# Controlled terrain material measurement

*State as of 2026-10-31.* This is the procedure for the M5 comparison of the
#333 terrain material, macro variation and parallax occlusion. It produces
evidence only when it is run on the target GPU; a worker does not invent a
result.

## Controls

- Host: same machine, same GPU and driver for every run. Record the driver
  version and the GPU name from the log.
- Window: `--no-editor --width 1920 --height 1080`. The log line from the
  swapchain must report a 1920x1080 extent; an editor-layout run is maximized
  and is not a controlled size.
- Scene: the `TerrainCheck` scene with the same recorded camera pose and no input. Keep
  post-processing and anti-aliasing fixed across raster and RT runs.
- Residency: let the streaming log settle before measuring. Wait until every
  requested tile and Region is resident and no upload or switch is pending;
  discard the warm-up frames.
- Frames: at least 30 warm-up frames, then at least 120 measured frames.
- Build: the same configuration for each pair of runs, `-O3` shaders in
  Release. Use Debug only to compare behavior, not absolute timings.

## Runs

For each configuration (raster and RT, material off/on where applicable):

~~~text
MjolnirEngine --game games/TerrainCheck --no-editor --width 1920 --height 1080 \
  --frames 150 --gpu-timing logs/material-334-<case>.csv
~~~

The CSV has `frame,region,width,height,milliseconds` rows. `frame` is the whole
renderer submission: uploads, shadows, TLAS, scene/trace, post, capture and UI,
ending before the submit; CPU and present waiting are outside it. `raster_scene`
is the scene render pass; `rt_trace_rays` is `vkCmdTraceRaysKHR` only and
excludes the TLAS build. `# summary,...` rows add count, minimum, median, p95 and
maximum. Rows are hardware timestamps polled after the GPU completes the frame
slot; no frame is idled to read them. The `frame` duration for a row must be at
least that row's `raster_scene` or `rt_trace_rays` duration.

## Reporting

- Repeat the measurement at least three times per case and report the median of
  each run's median and p95. Discard warm-up rows and the final capture/readback
  outliers from the timing statistics.
- Compare raster with raster and RT with RT. Report the 1920x1080 extent, the
  near/far clip distances and the driver, because they bound the ray cone and
  shadow cost.
- Images may confirm that shading changed; they are not timing evidence.
- Keep the CSVs and the log next to the report. A missing CSV, a non-1080p
  extent, an unsettled residency log or a different driver invalidates the
  comparison.

## Material controls

`parallaxSteps` bounds the height search (0 disables it even when
`parallaxEnabled` is true); `parallaxFadeStartMeters` and
`parallaxFadeEndMeters` fade the signed intersection offset smoothly with
distance, and `parallaxDistanceMeters` clamps it. `macroStrength` is the
variation amount, `macroPeriodMeters` the repeat period and
`parallaxRtEnabled` opts ray tracing into the same intersection.
