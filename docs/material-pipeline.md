# Surface material pipeline

*State as of 2026-10-31.* Shared surface materials and terrain rendering include
periodic macro variation and distance-faded parallax occlusion.

## Source contract

A surface material is one UTF-8 JSON object. Unknown root keys are preserved in
`SurfaceMaterialAsset::unknownJson` so an editor can save them back.

```json
{
  "version": 1,
  "shader": { "file": "materials/rock.slang", "type": "RockMaterial" },
  "frontends": ["mesh"],
  "parameters": { "albedoRoughness": [1, 1, 1, 0.8], "surfaceParams": [0.04, 1, 0, 0] },
  "textures": {
    "baseColor": { "path": "textures/rock_BaseColor.jpg", "colorSpace": "srgb" },
    "normal": { "path": "textures/rock_Normal.jpg", "colorSpace": "linear" },
    "displacement": { "path": "textures/rock_Displacement.jpg", "colorSpace": "linear" },
    "roughness": { "path": "textures/rock_Roughness.jpg", "colorSpace": "linear" },
    "specular": { "path": "textures/rock_Specular.jpg", "colorSpace": "srgb" },
    "ao": { "path": "textures/rock_AO.jpg", "colorSpace": "srgb" }
  },
  "resolution": 2048
}
```

- `version` must be 1; another value fails configuration or the cook.
- `shader.file` resolves from the owning assets root, then another approved
  engine/game assets root. It may not be absolute or climb out. `shader.type`
  is the Slang type passed to `-specialize`.
- `frontends` is a non-empty subset of `mesh` and `terrain`. Each declared
  frontend compiles its own raster, RT and reflected-layout registration.
- `parameters` is keyed by the reflected field name. Packing rejects a missing
  field, an extra shape or an unsupported type.
- `textures` is optional. When present, only the six roles above are accepted;
  each supplied role needs `path` and `colorSpace`. Missing roles use the packing
  defaults below. Without textures the material has no packed texture ids.
- `resolution` defaults to 2048 and selects the level-0 size of every packed
  output.
- A missing dependency or a Git LFS pointer in place of an image fails the cook
  and names the file; restore source content with `git lfs pull`.

The typed `SurfaceMaterialAsset` (tag `0x4d525355`, content version 1) is distinct
from the legacy glTF `MaterialAsset`. It stores the source contract and packed
texture asset ids in `albedoTexture`, `normalTexture` and `surfaceTexture`.
The runtime material library requests that payload through
`AsyncAssetService` and every cooked texture through the renderer's shared
`TextureLoadTracker`, keyed by the asset's content. It injects the ready texture
handles into the reflected `TextureRef` fields before creating the GPU material
on the main thread. The cooker preserves parameter JSON without interpreting
reflected fields.

## Packing

The six source roles map to three RGBA8 textures:

| Output | Format | R | G | B | A |
|---|---|---|---|---|---|
| `albedoTexture` | `RGBA8_SRGB` | BaseColor R | BaseColor G | BaseColor B | AO, linear |
| `normalTexture` | `RGBA8_UNORM` | normal X | normal Y | normal Z | 255 |
| `surfaceTexture` | `RGBA8_UNORM` | roughness | displacement | linearized specular (F0) | 255 |

Rules:

- BaseColor RGB is decoded from sRGB, filtered in linear light and re-encoded
  to sRGB. AO is always linear data, even when its source file or declared
  `colorSpace` says sRGB; it is filtered linearly and written to alpha.
- Normal X/Y/Z are decoded from the source OpenGL-convention normal, filtered
  as vectors and renormalized at every mip. A vector whose length collapses to
  near zero becomes the flat normal `(0, 0, 1)`.
- Roughness and displacement are linear values and are filtered without any
  height renormalization. Specular is decoded from sRGB to linear F0 and then
  filtered and stored as UNORM.
- Each output has a complete mip chain down to 1x1. An absent role contributes
  the constant default: white BaseColor/AO, flat normal, roughness 1,
  displacement 0.5, F0 0.04.

`TextureAsset` content version 2 stores `format`, `colorSpace`, the level-0
RGBA bytes and each higher mip's width, height, offset and length. Readers
reject an old version, a partial chain, a truncated payload or inconsistent
format/color-space metadata; the cooker's version fingerprint then forces a
recook. The renderer validates and uploads a supplied chain without generating
or replacing it.

## Incremental cook and editor

`AssetCooker` fingerprints the material JSON, its shader and every source image,
together with the payload versions and the material cooker version. Material
records live under `materials` in `import-cache.json` and carry their outputs,
dependencies, texture sizes, last success and last error. A full cook finds
materials under every source root; a partial cook accepts `--material` and maps
`--changed` source paths onto the material records that depend on them. A missing
or incompatible import cache forces a full recook; after a failed rebuild,
`requiresFullCook` keeps that migration pending until the new manifest publishes.

Material source images are inputs to the packed outputs, not standalone
textures. A full cook leaves them out, and a partial cook drops an older
standalone manifest entry for one. Manifest publication stays atomic with one
previous generation.

The editor passes the active game, shared engine and discovered game asset roots
to both full and partial batches, keeping the active root first for external
imports. It watches `.material.json`, images and shader sources. An edited
material enters a finite `--material` batch; an edited dependency enters a
`--changed` batch. A completed material-only batch still goes through the same
`PendingApply` path, so it is applied in Edit and waits for Stop during Play.

## Editor drafts

The Material panel builds its controls from the compiled reflection and keeps
one draft per normalized project and logical asset path. Numeric edits publish
the packed record through `SurfaceMaterialLibrary::PreviewParameters`; the
library overlays the values on the adopted parameters, runs the consumer
validator and updates the shared record table without creating a pipeline.
Raster and RT read the same record, so every use of the asset previews together.

A reflection revision the panel has not seen yet is adopted from the committed
generation, not rebuilt from the cooked asset: `VulkanRenderer::MaterialRecord`
returns the accepted layout and authoritative bytes of the live handle, and the
library decodes them into its cached parameters and its tile/macro/parallax
metadata. The cooked recipe stays the panel baseline; a field the new layout adds
takes the catalog's validated value, then the typed default, so a shader-only
field addition or deletion needs no recook. A recipe key the new layout no longer
declares stays in the source, stays reported as unsupported and is projected out
only when the record is packed. Previewed values never move the baseline.

One asset is one shared material across its fronts. An ordinary `MeshRenderer`
whose `materialShader` is a registered `mesh` stem borrows the library's handle;
the main scene and every Prefab preview resolver contribute their own active
uses, and the library merges them with the terrain's instead of one owner
replacing another. A pending edit publishes every valid frontend of the asset in
one batch: each frontend keeps its own layout and `TextureRef` rows, the overlay
is filtered to the names it declares, every record is validated and copied
before any is committed, and the commit itself does not allocate. A frontend
that joins later merges the peer's current same-name, same-kind numeric values
into its parameters before packing, so a value the other frontend rejects fails
the acquisition instead of publishing the cooked baseline; a cook candidate is
built from the cooked baseline only, and a failed cached entry retries on its
next acquisition. Shared finite, macro and parallax bounds apply to both
frontends, while the terrain UV phase divisor stays terrain-only. Revert queues
the accepted numeric values so a generation event cannot strand the GPU on the
dirty value.

Save writes only changed `parameters` and `textures.<role>.path` members through
`UpdateJsonFile`; unknown keys and the other texture fields stay. A successful
write queues one finite `--material` batch, and the ordinary Edit reload
publishes the new record. A failed batch keeps the last-good material and the
draft. Texture replacement changes only the source path, so packing, colour
space, resolution and mip generation are unchanged. Play is read-only.
## Material types

A material file defines only the surface function:

```slang
struct RockMaterial : IMaterial
{
    float4 albedoRoughness;
    TextureRef albedo;
    SurfaceOutput Evaluate(SurfaceInput input, MaterialContext context) { ... }
};
```

The pass supplies `SurfaceCommon.slang`; a material must not include it or
declare bindings. Sampling goes through `context.Sample(texture, uv)`.

## Building

`engine_add_surface_materials` recursively discovers `*.material.json` at
configure time for the engine and every game. A new material needs no wrapper
source and no CMake edit. The compile key is the normalized shader source path
plus type plus frontend; the output stem is
`<namespace>_<sanitized type>_<FNV-1a 32>` (`default` or the game directory for
the namespace). Assets sharing a source/type/frontend reuse the first compiled
stem, including across engine/game namespaces, while keeping separate catalog
records. Two same-named types from different sources get different
stems. Mesh and terrain registrations use separate frontend identities.

For the default type the outputs are:

| Output | Source | Entry | Specialization |
|---|---|---|---|
| `<stem>.vertexMain.spv` | `SurfaceForward.slang` | `vertexMain` | none |
| `<stem>.fragmentMain.spv` | `SurfaceForward.slang` + material | `fragmentMain` | `DefaultMaterial` |
| `<stem>.closestHitSurface.spv` | `SurfaceRt.slang` + material | `closestHitSurface` | `DefaultMaterial` |
| `<stem>.layout.spv` | `SurfaceLayout.slang` + material | `layoutMain` | `DefaultMaterial` |

Every output writes a reflection JSON and a depfile; the depfile is handed to
CMake as the command's `DEPFILE`, and the same argument array is written to
`shaders/shader_manifest.json`. `shaders/surface_materials.json` records each
asset's stem, type, frontend, normalized asset/shader paths, authored parameters
and reflection filename. A dependency of the executable synchronizes both files
next to it on every build with `copy_if_different`, including builds that only
change material parameters and do not relink the executable. The runtime reads
that catalog and the reflection JSON to pack records; it never
runs slangc and never reads the source checkout. Debug builds use `-g -O0`,
other configurations use `-O3` in both the command and the manifest.

Reflection and parameters are validated before allocation or writes: supported
field sizes, non-overlapping offsets, duplicate names, record bounds, JSON types,
finite floats and uint/int ranges. A malformed catalog, reflection or authored
parameter object returns a bounded error instead of throwing through renderer
startup. Missing fields remain an explicit failure.

## Development hot reload

The editor watcher reads only `shaders/shader_manifest.json` for the slangc
path, working directory and argument arrays. It follows each output's depfile,
coalesces saves, cancels a batch when a newer source revision exists and runs a
finite direct-slangc batch without CMake, vcvars or generated wrappers. A new
`.material.json` expands the shared mesh frontend template and is added to
`surface_materials.json`; the material store reloads the catalog so the new stem
gets raster and RT hit groups. See ADR 0007 (local ADR).

SPV, reflection, depfile and catalog files are staged as temporary siblings and
published only after every compile succeeds. The publisher retains per-file
backups and old stamps until the GPU transaction commits, so a rejected reload
restores the old files byte for byte. Stamps carry a source fingerprint and the
build-side publisher refuses to overwrite an output whose accepted fingerprint
is newer, so an older build copy cannot roll back a live reload. A non-editor
build links no watcher and reads no source, compiler or development path.

The reload transaction validates the whole batch first: SPIR-V magic and
entry-point stage, descriptor arrays, push ranges, UBO/SSBO member offsets and
strides, vertex/fragment interfaces including `SV_TARGET`. It then builds every
material, ray-tracing (whole pipeline and SBT), shadow and post-process
candidate off to the side. One device idle is taken and all candidates are
swapped only after every owner succeeds; otherwise the old generation stays
live. Material records migrate by field name: same-name compatible values are
preserved, new fields use the material's packed defaults and a changed type
rejects the transaction. The commit notifies the surface library without
allocating; it bumps its generation and adopts the committed layout and bytes the
next time the library is used, with no panel open.

## Default

`assets/materials/default.slang` is the default opaque material: albedo 0.5,
dielectric F0 0.04, roughness `pow(2/34, 0.25)`, AO on ambient only. It uses the
shared PBR lighting, so raster and RT agree. A public `MaterialDesc` whose
`shaderName` is the explicit `"default"` (as serialized by scenes) is normalized
to this catalog registration while preserving the caller's topology, culling,
alpha, sampler and explicit record. Devices without the shared tables keep the
legacy Blinn-Phong `assets/shaders/default.slang`; unknown surface requests
reject instead of falling back.

The default continues to ignore authored vertex colors. `SurfaceInput.vertexColor`
retains that raw data for materials which use it; the shared passes apply the
draw tint to albedo and emissive after evaluation, before lighting and guides.
The existing raster push block is visible to both vertex and fragment stages;
its size stays unchanged, and the command update uses the same shared stage mask.

## PBR and guides

Direct light uses `(diffuse + pi * specular) * lightColor * saturate(NdotL)`
for both the Sun and local lights; the pi cancels GGX's `1/pi` while preserving
the existing brightness convention. AO is ambient-only. RT specular albedo uses
the rational `EnvBRDFApprox2` directional-albedo fit documented in
[NVIDIA Streamline RR §4.2.1](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_RR.md#421-specular-albedo-generation),
evaluated with F0, squared roughness and the view cosine; the normal guide retains
linear roughness in `w`.

## Tangent frame

`TangentFrame.slang` is the single helper used by `SurfaceForward` and
`SurfaceRt`: it forms the tangent with an explicit sum of the model's linear
columns, orthogonalizes/normalizes it with a finite fallback, and flips
handedness for a mirroring determinant. Raster passes `SV_IsFrontFace`; RT
starts from `HitKind` and applies a per-instance correction for authored CW,
mirrored and front-cull settings, so both frontends agree. Materials orient
their normal from `frontFace` if they need a back-face response.

## RT hit groups

The RT pipeline has legacy camera and shadow hit groups at SBT offsets 0 and 1,
then a primary/shadow record pair for every catalog registration. A surface
instance's `instanceShaderBindingTableRecordOffset` is `2 + 2*i`, so
`TraceRay`'s primary offset 0 and shadow offset 1 land on that pair. A missing
catalog registration fails material creation instead of falling back to the
default shader. Alpha, double-sided and error/unlit/model materials remain on
the legacy path.

## Global illumination

DDGI keeps a single legacy hit-record pair, so a scene containing any
surface-material instance (nonzero SBT offset) disables DDGI and falls back to
the ordinary camera trace. Material-specialized DDGI pipelines are later work.

SSGI replays the legacy multi-target `<material>Surface` variants only. Generic
surface materials compile the single-target `SurfaceForward` frontend, so an
opaque generic surface draw keeps the existing raster lighting instead of
running the SSGI surface pass. Legacy default/model/unlit/error materials and
their SSGI inputs are unchanged.

## Terrain

A terrain material declares the `terrain` frontend. CMake compiles the same
material type through `SurfaceTerrainForward.slang` and `SurfaceTerrainRt.slang`
and keeps a separate registration for each frontend. Runtime terrain materials
are owned by the renderer's `SurfaceMaterialLibrary`, which loads the cooked
`SurfaceMaterialAsset`, resolves the catalog entry by asset and frontend, injects
packed texture rows into the reflected `TextureRef` fields using the exact names
`albedo`/`normal`/`surface` or their `Texture` aliases, packs the record and
creates the shared-table material. The terrain binding is a named layer list
containing only one base layer with weight one.

Terrain UVs are terrain-local meters. The mesh uploader reduces the mesh origin
once modulo the shared validation period and then adds each vertex locally, so a
triangle keeps its slope across the period. A material divides by its own
`tileSizeMeters`; its packed float value must divide the validation period
within the checked double arithmetic tolerance. The terrain material's `normalYUp` field flips the OpenGL
normal-map green channel. Region draw units use the coarse triangle's macro
normal and UV density, which is the documented Region normal limitation.

A cook reload commits the manifest, then the library starts asynchronous
replacements for the live assets. The old material and its texture claims stay
valid while a replacement decodes or uploads, and stay valid if it fails.
Frontends of one asset are prepared, validated and created together; any failure
keeps the whole previous generation. Superseded loads are dropped before they can
publish, unchanged content reuses shared texture uploads, and a failed attempt is
remembered until the next reload. Replaced handles are retired only after every
active mesh and terrain owner acknowledges the replacing generation. See
ADR 0004 (local ADR).

## Macro variation and parallax

A terrain material may add a reflected `TextureRef macro` plus
`macroPeriodMeters`, `macroStrength`, `macroHueShiftDegrees` and
`macroRoughnessContrast`. The macro texture is generated once through the
renderer's shared texture upload cache: it is deterministic, tileable,
low-frequency and shared by every material.
`uv0 / macroPeriodMeters` drives the extra sample; the period follows the same
strict packed-float divisibility rule as `tileSizeMeters`, and an authored zero
strength reproduces the ADR 0004 shading exactly.

Reflected `parallaxEnabled`, `parallaxRtEnabled`, `parallaxHeightMeters`,
`parallaxReference`, `parallaxDistanceMeters`, `parallaxSteps`,
`parallaxFadeStartMeters` and `parallaxFadeEndMeters` drive a shared
`SurfaceParallaxUv` intersection before the surface is sampled. It searches the
signed height range around the reference from high to low, so constant heights
above and below it produce opposite signed offsets independently of the step
count and a multicrossing profile takes the visible/frontmost crossing; the
fade scales that offset continuously and the distance bound clamps it. It uses the
original raster UV derivatives with `SampleGrad` and explicit `SampleLevel` in
ray tracing; RT parallax is off unless `parallaxRtEnabled` is true. The height
uses the stored displacement offset by the reference, not a renormalized range.
`SurfaceMacroContext` rescales raster gradients and RT footprint to the macro
period for the extra macro sample. Bounds are consumer-validated before the
material is published. See
ADR 0005 (local ADR).

## Integration with current pipeline owners

Transactional reload covers material, shadow, post-processing and camera RT
pipelines registered by this implementation. Current master also has SSGI/DDGI,
debug-drawing, composition and particle pipeline owners without replacement
callbacks. Their manifest outputs are content-pinned: changing, adding or
removing one rejects the entire live reload with a restart-required diagnostic.
Unchanged outputs do not block a supported material or post-process edit.
Reflection-only material layout outputs participate in record migration rather
than this pipeline-owner restriction. RTXGI SDK binaries retain their separately
packaged shader path and are not watched as Slang outputs.

### Shader compilation scheduling

The editor borrows App's shared JobExecutor and owns a BackgroundJobQueue with
concurrency one for shader compilation. Each batch compiles its affected outputs
sequentially; it does not create a separate worker pool. The engine's worker count
also controls this lane: zero workers executes compilation inline. Compilation
results and exceptions use BackgroundJobQueue::Submit and its standard future;
the watcher has no separate task-completion wrapper or completion flag. Results
cross back to the main-thread FrameBegin callback before publication and
GPU prepare/commit; pipeline replacement still waits for device idle. Shutdown
cancels queued shader work, signals active compiler cancellation, and waits only
for this lane before removing its owned artifacts. Other consumers of the shared
executor remain available.
