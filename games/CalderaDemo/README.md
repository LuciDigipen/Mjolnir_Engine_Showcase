# CalderaDemo

The Caldera island in Mjolnir: terrain, the airfield runway and buildings, and a procedural ocean.

## Cirrus SF50

The default scene includes `Cirrus SF50 - Arrivee Stellar` on the runway, with Family seating and the gear held extended. Entity 1144 loads `assets/models/sf50/sf50.glb`. Only the landing gear, bay doors, and linkages are animated; the other aircraft components are static.

The optimized aircraft uses 2,283,694 triangles and 162 material primitives, down from 3,158,827 and 263. Selective interior decimation preserves stitching, decals, instruments and small parts. Engine/bay/gear hardware shares one untextured machinery material; wheel materials and gear-door livery remain. Exterior geometry, gear skin bindings and animation samples are unchanged. The gear retains its earlier reduction to 138,758 triangles, down from 1,129,884. This does not add aircraft LODs or cache frozen gear poses. Rebuild/reimport after an export change to use the updated runtime model.

The editable master is `source/aircraft/sf50/cirrus_sf-50.blend`, with adjacent `sf50_textures/`, `reports/`, and `previews/` folders. This authoring package stays outside the runtime cook roots. The material/interior pass updates both the master and runtime GLB; source texture files are unchanged. The export uses a single gear clip and limits runtime base-color maps to 1024 pixels (2048 for fuselage, wings, and livery). See `source/aircraft/sf50/reports/sf50_material_interior_review.md` for measurements and validation.

The model-rendering path currently uses core glTF base color, vertex colors, and alpha. Scalar metallic/roughness values are retained in glTF, but this importer does not consume them yet. Windows use a simple alpha-blended approximation; normal maps, clear coat, and the master's Fresnel glass are not reproduced by that path. Export and scene reports are in `source/sf50_export_report.json` and `source/sf50_scene_report.json`. The latter records the actual exported clip duration used for the parked pose.

## Getting it

The large terrain/model files and SF50 Blender source/textures are in Git LFS and download with the rest of the repository once `git lfs install` has run on the machine (see the root README). Missing runtime content leaves pointers, and CMake leaves the game out with a warning until it is fetched. From the repository root:

```text
git lfs pull
```

The next build reconfigures and includes the game; then run `MjolnirEngine --game games/CalderaDemo`. The first build on a machine bakes terrain, about 16 minutes in Release and longer in Debug, since TerrainTool is built in the same configuration. The game needs about 6 GiB of VRAM.

## Licenses

**Caldera data.** The runway and airfield models under `assets/models/`, `assets/meshes/`, `assets/terrain/`, `source/caldera.heights`, and the building and runway placements in `scenes/` are modified from and derived from Activision's Caldera data set, Copyright © 2024, 2025 Activision Publishing, Inc., released under the Activision Non-Commercial Use License Agreement. `LICENSE.md` is a copy of it. Non-commercial use only. The separately supplied SF50 under `assets/models/sf50/` is not derived from the Caldera dataset.

Modifications: meshes were converted to glTF with new names, placement and vertex colors, and interior objects were removed. Heights were merged, filled, resampled and stamped under the runway. The ocean mesh is computed from those heights. Tools are in `tools/caldera_export/`.

The license also names a `Notice` file. The data set, at the commit these files came from (`03feaa188`), has none within three folder levels of its root, so none is included.

**Sand textures.** `assets/textures/icelandic_sand_and_rocks/` are Fab purchased assets, for project collaborators only, under the Fab EULA: https://www.fab.com/eula?lang=en.

**Concrete texture.** `source/textures/concrete_color.jpg` is CC0; see the README beside it. The runway model embeds it.
