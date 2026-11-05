# CalderaDemo

The Caldera island in Mjolnir: terrain, the airfield runway and buildings, and a procedural ocean.

## Getting it

Its large files, about 1.5 GiB, are in Git LFS and download with the rest of the repository once `git lfs install` has run on the machine (see the root README). A clone from before that holds pointers, and CMake leaves the game out with a warning until they are fetched. From the repository root:

```text
git lfs pull
```

The next build reconfigures and includes the game; then run `MjolnirEngine --game games/CalderaDemo`. The first build on a machine bakes terrain, about 16 minutes in Release and longer in Debug, since TerrainTool is built in the same configuration. The game needs about 6 GiB of VRAM.

## Licenses

**Caldera data.** Everything under `assets/models/`, `assets/meshes/`, `assets/terrain/`, `source/caldera.heights`, and the building and runway placements in `scenes/` is modified from and derived from Activision's Caldera data set, Copyright © 2024, 2025 Activision Publishing, Inc., released under the Activision Non-Commercial Use License Agreement. `LICENSE.md` is a copy of it. Non-commercial use only.

Modifications: meshes were converted to glTF with new names, placement and vertex colors, and interior objects were removed. Heights were merged, filled, resampled and stamped under the runway. The ocean mesh is computed from those heights. Tools are in `tools/caldera_export/`.

The license also names a `Notice` file. The data set, at the commit these files came from (`03feaa188`), has none within three folder levels of its root, so none is included.

**Sand textures.** `assets/textures/icelandic_sand_and_rocks/` are Fab purchased assets, for project collaborators only, under the Fab EULA: https://www.fab.com/eula?lang=en.

**Concrete texture.** `source/textures/concrete_color.jpg` is CC0; see the README beside it. The runway model embeds it.
