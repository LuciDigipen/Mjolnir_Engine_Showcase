# ADR 0002: Generic surface material framework and default PBR

- Status: Accepted
- Date: 2026-10-26
- Issue: #331

## Context

`#330` added shared raster/RT material tables, stable image/sampler rows and an
opaque record table. There was still no common surface layer: raster materials
owned a full vertex+fragment pair, RT lighting sampled a legacy Blinn-Phong
constant, and the material record's field layout had no producer.

The v4 terrain material design chose generic specialization: a material is a
type implementing one surface function, the frontend and hit group are fixed
engine code, and slangc specializes an entry point with `-specialize`.

## Decision

1. The surface contract is Slang source in `assets/shaders/SurfaceCommon.slang`:
   `SurfaceInput`, `SurfaceOutput`, `IMaterial`, `MaterialContext`,
   `MeshFrontend` and `TextureRef`. Materials declare no resource bindings.
2. `SurfaceForward.slang` owns the raster vertex stage and a material-generic
   fragment stage. `SurfaceRt.slang` owns a material-generic closest-hit stage.
   Both call the shared `ShadePbr` (Lambert + GGX/Smith/Schlick, no `/pi`:
   `lightColor` already carries the existing brightness convention).
3. Raster reads the material record through set 0's dynamic `MaterialRecordRef`
   and set 4's byte-address buffer. RT reads the same bytes through
   `RtInstanceInfo.materialRecord` and set 0's binding 11. The default material
   record is `DefaultSurfaceMaterial` in `renderer/Material.h`; a compiled Slang
   reflection test keeps its two `float4` fields at offsets 0 and 16.
4. A raw `ByteAddressBuffer.Load<M>` reflection does not expose `M`'s fields.
   `SurfaceLayout.slang` compiles the same type into a
   `StructuredBuffer<M>` witness, and `MaterialReflection` turns its
   `resultType.fields` into offsets and a 16-byte-rounded record size. This is
   the only field table; there is no handwritten duplicate.
5. The default material is a surface type (`assets/materials/default.slang`).
   Its PBR constants preserve the old look: albedo 0.5, F0 0.04, roughness
   `pow(2/34, 0.25)`, AO on ambient only. A legacy `default.slang` remains as
   the fallback for devices without shared material tables.
6. CMake discovers `materials/*.material.json` under the engine assets root and
   each game's assets root, validates the shared v1 schema, deduplicates by
   `<namespace>_<type>`, and compiles vertex, fragment, RT and layout outputs
   through the frontend templates. The namespace is `default` for engine
   materials and the game directory name for game materials.
7. The same CMake variables write `shaders/shader_manifest.json`: slangc path,
   working directory, common argument array, one record per output (sources,
   entry, stage, type, specialization, output, reflection, depfile, arguments)
   and the mesh frontend templates. Legacy shaders are in the same manifest.
8. Every output compiles with `-reflection-json` and `-depfile`; CMake receives
   the depfile through the `DEPFILE` source property, so a material source edit
   rebuilds its outputs without a wrapper source.
9. RT keeps the legacy hit group at SBT offset 0 and the shadow any-hit group at
   1. Surface-material instances use offset 2, a distinct closest-hit group
   compiled from `SurfaceRt.slang`; the legacy alpha any-hit and double-sided
   rules stay in `raytrace.slang`. A material without shared tables stays on the
   legacy raster default.
10. `MaterialContext` hides the only raster/RT difference: raster uses
    `SampleGrad` with screen derivatives, RT uses `SampleLevel` until ray
    differentials exist. `TextureRef` carries stable image and sampler rows.

## Coordinate spaces

- `positionRS`, `normalWS`, `tangentWS`, `viewDirWS`: camera-relative render
  space. World axes, camera at the origin; never double-precision world space.
- `localPos`: the drawn mesh's object space.
- `uv0`: backend-defined. `MeshFrontend` uses the artist UV.
- `bitangentSign`: the vertex tangent's handedness, unchanged from the asset.
- `frontFace`: the raster `SV_IsFrontFace` result; RT derives the same test from
  the ray direction and the instance's double-sided flag.

## Frame and refresh metadata

`MaterialLayout` is the refresh unit for a future parameter panel and SPV
reload: field name, kind, offset and size, plus the aligned record size. A
material type's reflection JSON and the manifest output record are keyed by the
same namespaced output stem. A shader reload can compare descriptor layouts,
recompile with the manifest command, and repack the record from the same field
names without a second parameter declaration.

## Prototype evidence

The supervisor probe in `builds/material-prototype-331/` compiled a separate
pass and material with `slangc -specialize`, found offsets 0/16/28 through the
typed witness, and read 3.25 back from a Vulkan compute dispatch on an RTX 3070
Laptop. The native-layout negative control failed as expected.

Reproduce from the main checkout:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File builds/material-prototype-331/run-prototype.ps1
PASS GPU=NVIDIA GeForce RTX 3070 Laptop GPU value=3.25 expected=3.25 offsets=0,16,28
FAIL GPU=NVIDIA GeForce RTX 3070 Laptop GPU value=1.05696e+09 expected=3.25 offsets=0,16,28
```

The pass loaded `ProbeMaterial` from a `ByteAddressBuffer`; the layout came
from a separate `StructuredBuffer<M>` witness because the raw buffer reflection
has no fields. The same `-fvk-use-gl-layout` flag is used for every surface
specialization here. The `SurfaceMaterialLayout` test compiles the real default
type and checks the packed bytes against the C++ record. GPU pixel checks
remain supervisor work.
