# Surface material pipeline

*State as of 2026-10-26.* M2 (#331) defines the surface framework, the default
PBR material, the `.material.json` schema, shader discovery and the shader
manifest. Texture cooking and the terrain frontend are later issues.

## Source contract

A surface material is one UTF-8 JSON object. Unknown root keys must be preserved
by an editor writing it back.

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

- `version` must be 1; another value fails configuration.
- `shader.file` resolves from the owning assets root, then another approved
  engine/game assets root. It may not be absolute or climb out. `shader.type`
  is the Slang type passed to `-specialize`.
- `frontends` is a non-empty subset of `mesh` and `terrain`. M2 compiles mesh
  outputs; a terrain-only material is valid but waits for the terrain frontend.
- `parameters` is keyed by the reflected field name. Packing rejects a missing
  field, an extra shape or an unsupported type.
- `textures`, when present, uses exactly the six roles above. Missing textures
  are valid for a constant material.

The typed `SurfaceMaterialAsset` (tag `0x4d525355`, content version 1) is owned
by the M3 asset work; M2 only parses the source contract for compilation and
packing. The packed outputs map to the reflected `albedoTexture`,
`normalTexture` and `surfaceTexture` fields.

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
the namespace). Two assets sharing a shader compile once while keeping separate
catalog records; two same-named types from different sources get different
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
and reflection filename. Both files are copied next to the executable. The
runtime reads that catalog and the reflection JSON to pack records; it never
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
linear roughness in `w`. `PbrProbe.slang` covers the analytic F0=0 Lambert
oracle (`0.5 * NdotL`, including multiple angles), back-face zero, finite
grazing and guide roughness/F0 dependence. The probe is compiled and
spirv-val checked here; its GPU readback values remain supervisor evidence.

## Tangent frame

`TangentFrame.slang` is the single helper used by `SurfaceForward` and
`SurfaceRt`: it forms the tangent with an explicit sum of the model's linear
columns, orthogonalizes/normalizes it with a finite fallback, and flips
handedness for a mirroring determinant. Raster passes `SV_IsFrontFace`; RT
starts from `HitKind` and applies a per-instance correction for authored CW,
mirrored and front-cull settings, so both frontends agree. Materials orient
their normal from `frontFace` if they need a back-face response.
`TangentFrameProbe.slang` covers a rotation plus non-uniform scale, a mirror
and a degenerate tangent for supervisor GPU readback.

## RT hit groups

The RT pipeline has legacy camera and shadow hit groups at SBT offsets 0 and 1,
then a primary/shadow record pair for every catalog registration. A surface
instance's `instanceShaderBindingTableRecordOffset` is `2 + 2*i`, so
`TraceRay`'s primary offset 0 and shadow offset 1 land on that pair. A missing
catalog registration fails material creation instead of falling back to the
default shader. Alpha, double-sided and error/unlit/model materials remain on
the legacy path.
