# SF50 performance investigation

This is the pre-change investigation. The completed changes and current counts
are in [sf50_material_interior_review.md](sf50_material_interior_review.md).

Inspected the open source file using the official Blender Authors MCP extension
on localhost:9876, Blender 5.2.1 LTS. The current runtime GLB was independently
inspected. No source geometry, saved blend, runtime GLB, or engine code was changed.
Temporary decimation objects were removed after measurement.

## Current asset

- GLB: 198,654,500 bytes (189.45 MiB), 3,158,827 triangles, 2,086,152 exported vertices.
- 16 meshes, 263 material primitives, 168 materials, 106 embedded PNG images.
- Only the three landing gear objects are skinned: 138,758 triangles and 129,901
  exported vertices. The other 3,020,069 triangles are static.
- Seats total 1,109,897 triangles (35.1% of the aircraft).
- Exported vertex counts exceed Blender vertex counts because attribute seams
  and material primitive boundaries split vertices; this is expected, not by itself
  evidence of duplicate surfaces.
- Embedded images: 93 at 1024 square, 8 at 2048 square, 4 at 512 square, one
  at 1024x256. All 106 decoded to RGBA8 total 505 MiB without mipmaps, approximately
  673 MiB with full mip chains. This is an aggregate image-size estimate, not
  measured GPU residency; shared references, actual loading and allocation matter.

## Temporary decimation experiments

Collapse decimation, triangulation enabled, no permanent modifier application.
Distances sample vertices and triangle centroids in world meters, in both
directions, approximately 8,192 samples per direction per experiment.
Sampling can miss small defects. These measurements do not validate UVs,
normals, thin decals, silhouettes, seams, or material boundaries.

| Object | Before | Candidate | Ratio | Source-to-candidate p99 / max (mm) | Candidate-to-source max (mm) |
|---|---:|---:|---:|---:|---:|
| Seats - rear | 429,513 | 107,378 | .25 | .297 / 1.731 | .713 |
| Seats - family | 331,566 | 82,891 | .25 | .278 / 2.636 | .393 |
| Gear bays | 313,991 | 78,496 | .25 | .170 / .361 | .307 |
| Fuselage details | 251,230 | 62,807 | .25 | .307 / .594 | .437 |
| Cabin and cockpit | 595,111 | 148,776 | .25 | .297 / .679 | .745 |
| Cabin and cockpit | 595,111 | 297,554 | .50 | .066 / .725 | .174 |
| Seats - cockpit | 348,818 | 87,203 | .25 | .230 / 2.432 | 17.553 |
| Seats - cockpit | 348,818 | 34,880 | .10 | .789 / 3.196 | 21.590 |

Rear/family seat .10 experiments showed larger sampled errors (up to 7.4 mm),
so .25 was a better starting point for the initial feasibility investigation.

The five .25 candidates excluding cockpit seats remove 1,441,063 triangles,
leaving 1,717,764 total: a 45.6% reduction. This is a feasibility result, not
a validated production asset or an FPS improvement measurement.

## Recommended asset work, in order

1. Start with gear bays and fuselage details, then rear/family seats. Protect
   decals, material borders, UV seams and small hardware before applying any
   reduction; compare textured renders and silhouettes at intended camera distances.
   Transfer custom normals where necessary and remeasure the protected result.
2. Simplify cabin geometry by material/connected component. Preserve instrument
   text, screens, switch shapes and thin overlays. The whole-object .25 result
   is a useful upper bound on savings, not approval to collapse all these parts.
3. Handle cockpit seats selectively. The reverse-distance failures above reject
   naive whole-object reduction for close cockpit views, despite low forward errors.
4. Build distinct exterior and cockpit representations. The current glTF scene
   includes the full interior even for an outside camera. Split sensible sections
   for visibility and distance control; do not assume the fuselage automatically
   prevents interior submission or ray-tracing work. Open doors/windows need care.
5. Add authored LODs plus runtime distance/projected-size selection and hysteresis.
   Exporting several LOD meshes into the same active glTF scene would draw all of
   them with the current engine. Separate model assets or explicit scene selection
   can provide an initial manual mechanism; automatic LOD needs engine work.
6. Deduplicate equivalent engine-visible materials, then merge compatible
   primitives within spatial sections. Joining objects alone does not remove
   material splits. Texture atlases can reduce splits further but require UV work.
   Retain independent gear skinning and transparent surfaces.
7. Review texture size by projected texel density; downsize hidden/interior detail
   for exterior LODs. The GLB is already mostly 1K, so geometry and submission are
   stronger first targets than indiscriminate texture resizing.

## Engine and glTF handling

Findings below come from the current repository implementation, not assumed glTF behavior.

- `tools/AssetCooker/GltfImporter.cpp`: preserves imported primitive geometry;
  no simplification, automatic LOD generation, or primitive merging occurs.
  Each glTF primitive remains a separate mesh primitive. PNG/JPEG images decode
  during cook. glTF is not parsed in the frame loop.
- That importer decodes and emits every embedded image, but current material
  import uses base color/factor, sampler, alpha, and double-sided settings.
  Normal/metallic/roughness material channels are not wired by this import path.
  Baking geometric detail into normals therefore requires importer/material support
  before it can replace visible geometry correctly in this engine.
- `modules/animation/src/animation/ModelSystem.cpp`: submits one item per visible
  scene-node primitive. `VisibleModelNodes` selects glTF scene hierarchy membership;
  it is not camera occlusion, interior filtering, or automatic LOD.
- `modules/renderer/src/renderer/vulkan/ModelDescriptors.cpp`: creates an upload
  buffer and descriptor set per model draw per frame, including static draws
  (a small material payload). 263 primitives therefore create significant CPU
  allocation/submission work even after triangle reduction. Pool/arena allocations,
  dynamic offsets and persistent immutable deformation data are useful engine work.
- `modules/animation/src/animation/ModelAnimation.cpp`, `BuildDeformationData`:
  rebuilds deformation payloads including immutable influence ranges and weights.
  Cache immutable data on the GPU and upload only changing pose/morph values.
- `modules/renderer/src/renderer/vulkan/RayTracerDeformation.cpp`: allocates
  deformation/output buffers and builds a BLAS for each deformed primitive each
  frame; old resources are destroyed when the frame slot resets. Only the three
  gear meshes trigger skin deformation for this asset, across 53 primitives.
  Cache unchanged poses, retain buffers, and consider BLAS update/refit where
  supported and worthwhile. Do not rebuild stationary gear every frame.
- Draco/meshopt/KTX2 are outside the documented importer support. Adding compressed
  GLB extensions alone is not a runtime geometry optimization and can fail cooking
  if required extensions are unsupported. GPU texture compression needs explicit
  cooker and runtime format support.

## Verification required before shipping

Compare fixed cockpit, close exterior, medium and distant camera views using
the same engine settings/resolution. Record CPU/GPU frame milliseconds, draw
counts, geometry, texture residency, loading peaks and RT acceleration-structure
time with RT both on and off. Include animated and stationary gear poses.
Compare textured renders, UV continuity, custom normals and instrument readability;
check gear through its full animation after any skinned edit. Re-export, cook and
verify skin/clip/material identities and counts. No engine FPS benchmark was run
in this investigation, so a frame-rate guarantee cannot be made from triangle counts.
