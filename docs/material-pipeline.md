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

`SurfaceMaterialAsset` (tag `0x4d525355`, content version 1) is the typed
payload. It is distinct from the legacy glTF `MaterialAsset`. Its packed
references are `albedoTexture`, `normalTexture` and `surfaceTexture`.

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

`engine_add_surface_materials` discovers `materials/*.material.json` at
configure time for the engine and every game. A new material needs no wrapper
source and no CMake edit. The output stem is `<namespace>_<type>`, where the
namespace is `default` or the game directory. For `default_DefaultMaterial`:

| Output | Source | Entry | Specialization |
|---|---|---|---|
| `default_DefaultMaterial.vertexMain.spv` | `SurfaceForward.slang` | `vertexMain` | none |
| `default_DefaultMaterial.fragmentMain.spv` | `SurfaceForward.slang` + material | `fragmentMain` | `DefaultMaterial` |
| `default_DefaultMaterial.closestHitSurface.spv` | `SurfaceRt.slang` + material | `closestHitSurface` | `DefaultMaterial` |
| `default_DefaultMaterial.layout.spv` | `SurfaceLayout.slang` + material | `layoutMain` | `DefaultMaterial` |

Every output writes a reflection JSON and a depfile. `shaders/shader_manifest.json`
records the complete commands, argument arrays and mesh frontend templates, and
is copied next to the executable for the future editor reload path.

## Default

`assets/materials/default.slang` is the default opaque material: albedo 0.5,
dielectric F0 0.04, roughness `pow(2/34, 0.25)`, AO on ambient only. It uses the
shared PBR lighting, so raster and RT agree. Devices without the shared tables
keep the legacy Blinn-Phong `assets/shaders/default.slang`.

## RT hit groups

The RT pipeline has legacy camera and shadow hit groups at SBT offsets 0 and 1,
plus the default surface material at offset 2. Surface instances set
`instanceShaderBindingTableRecordOffset` from their material. Shadow rays keep
offset 1, whose any-hit test reads the instance's alpha state; alpha, double-sided
and error/unlit/model materials remain on the legacy path.
