# Surface material pipeline

*State as of 2026-10-26.* The v1 `.material.json` contract and its packed
textures are implemented by `assets` and `tools/AssetCooker`. A real surface
shader and the renderer-side material binding remain M4/#330 work.

## Source contract

A surface material is one UTF-8 JSON object. Unknown root keys are preserved in
`SurfaceMaterialAsset::unknownJson` so an editor can save them back.

```json
{
  "version": 1,
  "shader": { "file": "shaders/surface.slang", "type": "opaque" },
  "frontends": ["terrain"],
  "parameters": { "roughnessBias": 0.0 },
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

- `version` must be `1`; another value fails the cook with a diagnostic.
- `shader.file` and `shader.type` are non-empty strings. `file` must exist under
  the owning assets root or another approved engine/game assets root. `type` is
  an opaque shader identifier for the runtime material layer; the cooker does
  not infer one.
- `frontends` is a non-empty subset of `mesh` and `terrain`.
- `parameters` is an object keyed by the future reflected field name. The
  cooker stores it without interpreting it.
- `textures` is optional. When absent the material is a valid constant
  material with no packed texture ids. The six roles are the only accepted
  keys; each needs `path` and `colorSpace`.
- `resolution` is optional and defaults to 2048. It is the level-0 size of each
  packed output and is useful for tests.
- Paths are lexical asset paths, never process-relative paths. A path is
  resolved from the material's owning assets root first, then from the other
  configured engine/game roots. It may not be absolute or climb out of an
  approved root.
- A missing dependency, or a Git LFS pointer left in place of a source image,
  fails the cook and names the file. Run `git lfs pull` to restore the content.

`SurfaceMaterialAsset` is a distinct typed payload:
`kSurfaceMaterialAssetTypeTag` with content version 1. It is not the legacy glTF
`MaterialAsset`. Its packed references are `albedoTexture`, `normalTexture` and
`surfaceTexture`; no arbitrary channel graph is inferred.

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
`--changed` source paths onto the material records that depend on them.

Material source images are inputs to the packed outputs, not standalone
textures. A full cook leaves them out, and a partial cook drops an older
standalone manifest entry for one. Manifest publication stays atomic with one
previous generation.

The editor watches `.material.json`, images and shader sources. An edited
material enters a finite `--material` batch; an edited dependency enters a
`--changed` batch. A completed material-only batch still goes through the same
`PendingApply` path, so it is applied in Edit and waits for Stop during Play.
