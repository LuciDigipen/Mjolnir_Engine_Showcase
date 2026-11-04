# CalderaDemo

The Caldera island in Mjolnir: terrain, the airfield runway and buildings, and a procedural ocean.

## Getting it

Clones skip this game's large files by default (see `.lfsconfig`), and CMake leaves the game out while they are missing. To opt in, from the repository root:

```text
git config lfs.fetchexclude ""
git lfs pull
```

The local setting keeps later checkouts from turning these files back into pointers. Reconfigure and build, then run `MjolnirEngine --game games/CalderaDemo`. The first build on a machine bakes terrain, about 16 minutes in Release. The game needs about 6 GiB of VRAM.

## Licenses

**Caldera data.** Everything under `assets/models/`, `assets/meshes/`, `assets/terrain/`, `source/caldera.heights`, and the building and runway placements in `scenes/` is modified from and derived from Activision's Caldera data set, Copyright © 2024, 2025 Activision Publishing, Inc., released under the Activision Non-Commercial Use License Agreement. `LICENSE.md` is a copy of it. Non-commercial use only.

Modifications: meshes were converted to glTF with new names, placement and vertex colors, and interior objects were removed. Heights were merged, filled, resampled and stamped under the runway. The ocean mesh is computed from those heights. Tools are in `tools/caldera_export/`.

The license also names a `Notice` file. The data set, at the commit these files came from (`03feaa188`), has none within three folder levels of its root, so none is included.

**Sand textures.** `assets/textures/icelandic_sand_and_rocks/` are Fab purchased assets, for project collaborators only, under the Fab EULA: https://www.fab.com/eula?lang=en.

**Concrete texture.** `source/textures/concrete_color.jpg` is CC0; see the README beside it. The runway model embeds it.
