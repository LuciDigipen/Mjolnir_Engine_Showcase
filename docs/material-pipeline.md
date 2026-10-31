# Surface material pipeline

*State as of 2026-10-30.* M2 (#331) defines the surface framework, the default
PBR material, shader discovery and the shader manifest. M3 (#332) adds typed
material assets, packed texture cooking and versioned mip chains. The terrain
frontend and runtime binding of the packed surface textures remain later work.

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
- `frontends` is a non-empty subset of `mesh` and `terrain`. M2 compiles mesh
  outputs; a terrain-only material is valid but waits for the terrain frontend.
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
Those ids describe cooked outputs; resolving them into reflected `TextureRef`
fields and the terrain material is later integration work. The cooker preserves
parameter JSON without interpreting reflected fields.

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
stems. Terrain-only declarations defer without suppressing a later mesh one.

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
