# SF50 asset workflow

Run from the repository root with Blender 5.2 installed. The editable source lives outside the asset cook roots; only `games/CalderaDemo/assets/models/sf50/sf50.glb` and its `.meta` belong to the runtime model package.

```powershell
$blender = 'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe'
$source = 'games/CalderaDemo/source/aircraft/sf50/cirrus_sf-50.blend'

# Check the rig, animation, static geometry, and all texture files/hashes.
& $blender --background $source --python tools/sf50/validate.py

# Export a complete GLB atomically, retaining its existing .meta identity.
& $blender --background $source --python tools/sf50/export_caldera.py
```

Then reimport the GLB in the editor or rebuild the engine to cook/copy it. The export retains the gear action and converts materials to the model shader's core glTF base-color/alpha approximation; it does not save changes to the Blender master. Add `-- --render` to validation to regenerate the exterior and retracted-gear previews without changing the master.

`place_in_caldera.py` validates the GLB and recreates the initial runway placement, including the exported clip's extended-gear time. Run it with Blender's bundled Python only when deliberately creating/resetting that placement; it overwrites the existing SF50 entity's transform.

`optimize_gear.py` reproduces the geometry reduction from a full-detail checkpoint. It rejects an already optimized master, leaves the input file intact, and writes candidates/measurements under ignored `builds/sf50_gear_optimization/` for review. Local full-detail backups are under ignored `builds/sf50_backups/`; restore a checkpoint beside the source textures before running the optimizer.

Keep `.glb.meta` in version control. It stores the model GUID and stable sub-asset keys; deleting it would change runtime asset identities. Git LFS rules cover the runtime GLB, editable blend, and authoring PNGs.
