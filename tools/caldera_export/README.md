# Caldera export

Turns the Caldera USD dataset into what `games/CalderaDemo` needs. The engine and `TerrainTool` never link OpenUSD; USD stays in these scripts. See `docs/terrain/caldera-import-plan.md` for the reasoning and measurements.

## Environment

Python with the pinned packages in `requirements.txt`. `usd-core` provides `pxr`.

```text
uv venv .venv
uv pip install --python .venv/Scripts/python.exe -r tools/caldera_export/requirements.txt
```

Run the scripts without `-I`, because they import `export_terrain` from their own folder. The expected dataset is Caldera commit `03feaa188`; `export_terrain.PINNED_COMMIT` holds the full hash, and the reports record the commit they ran from.

## Commands

Run from the repository root. `--caldera-root` is the dataset's checkout.

| Script | Command | Writes |
|---|---|---|
| Terrain | `python tools/caldera_export/export_terrain.py --caldera-root <dir> --game-dir games/CalderaDemo [--sea-level 0]` | `source/caldera.heights`, the terrain recipe, a report |
| Runway | `python tools/caldera_export/export_runway.py --caldera-root <dir> --game-dir games/CalderaDemo` | `assets/models/runway.glb`, a report |
| Buildings | `python tools/caldera_export/export_buildings.py --caldera-root <dir> --game-dir games/CalderaDemo [--keep-interior-objects]` | `assets/models/airfield/*.glb`, the LOD script, scene entities, a report |
| Ocean | `python tools/caldera_export/export_ocean.py --game-dir games/CalderaDemo [--sea-level 1.2] [--cell 8]` | `assets/meshes/ocean.mesh.json`, a report |

Run terrain first: the ocean reads its heights and needs no USD. Then build, which bakes terrain when the heights changed and cooks the models.

- **Heights change:** a rebake follows, about 16 minutes in Release.
- **Ocean level:** pass the same level to `export_ocean.py` and to the ocean entity's Y in the scene. Neither rebakes terrain. Terrain's own `--sea-level` is separate, and it rebakes.
- **A failed build:** the engine reads `cooked/`, which a late build step publishes from `cooked_assets/`, so a build that fails after the cook leaves the old models running.

## Tests

Each exporter has a test on synthetic data, run from this folder:

```text
python test_export_terrain.py
python test_export_runway.py
python test_export_buildings.py
python test_export_ocean.py
```

## License

Caldera's data is under Activision's Non-Commercial Use License (`LICENSE.md` in the dataset). Everything exported from it stays under those terms: heights, the runway, the buildings and the ocean mesh, whose depths derive from the heights. `games/CalderaDemo/` is git-ignored for this reason. Confirm the terms before any public showing.
