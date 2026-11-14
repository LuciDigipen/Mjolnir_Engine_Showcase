# SF50 material consolidation and interior reduction

Both the Blender master and runtime GLB are updated. The original files and
`.meta` were backed up locally before optimization.

| Measurement | Before | After | Reduction |
|---|---:|---:|---:|
| Triangles | 3,158,827 | 2,283,694 | 27.7% |
| Material primitives | 263 | 162 | 38.4% |
| Used runtime materials | 168 | 92 | 45.2% |
| Embedded runtime images | 106 | 69 | 34.9% |
| GLB bytes | 198,654,500 | 133,737,372 | 32.7% |
| Animated gear primitives | 53 | 16 | 69.8% |

## Material changes

- One shared untextured machinery material replaces engine, CAPS-compartment,
  gear-bay and non-wheel animated gear hardware materials. Engine and gear bays
  each export as one primitive rather than 15.
- Rig-weight membership identifies wheel/tire faces, including wheel faces whose
  materials also appeared on suspension parts. Their original materials remain.
  Gear-door exterior livery is retained.
- Bare metal, non-painted hardware and bolts on the exterior share the machinery
  material; exterior paint, livery, rubber and glass remain distinct.
- 26 identical complete shader-graph aliases are merged. Existing normal inputs
  are included in the equivalence checks, preserving distinctions needed for
  future normal-map support. Visible upholstery and instrument surfaces remain.
- Unused authoring material datablocks are retained in the master as references;
  they do not create runtime primitives or get exported as used materials.

The common material is authored with metallic .85 and roughness .48. These
scalars survive glTF export, but the current model importer renders only base
color/vertex color/alpha. Metallic shading is not implemented by this asset pass.

## Interior geometry

| Object | Triangles before | Triangles after | Worst sampled bidirectional deviation |
|---|---:|---:|---:|
| Rear seats | 429,513 | 223,548 | .472 mm |
| Family seats | 331,566 | 175,178 | .542 mm |
| Cockpit seats | 348,818 | 198,052 | .442 mm |
| Cabin and cockpit | 595,111 | 345,214 | 1.223 mm |
| Cabin doors | 110,198 | 56,726 | .238 mm |
| Cargo compartment | 125,250 | 66,605 | .306 mm |
| Total changed interior | 1,940,456 | 1,065,323 | 45.1% fewer triangles |

The reduction protects decals, stitching, instruments, panels, glass, small
material regions and small disconnected components. Further components that
failed surface checks were protected and the candidate regenerated. Remaining
dense surfaces use a .35 collapse ratio. Source corner normals are transferred
to reduced geometry. UV layers and material assignments are retained; no new
UV atlas or texture bake was introduced.

Accepted candidates preserve connected-component counts and Euler characteristics,
do not increase nonmanifold-edge counts, and introduce no new degenerate faces.
The cabin already had three degenerate faces; this pass does not silently repair
unrelated original topology. Surface checks sample vertices and triangle
centroids in both directions, roughly 32,000 points per direction. Material
interfaces are checked separately when shared vertices form an interface.
Acceptance limits are 3 mm maximum and 1 mm at p99.9, with 0.05 mm at those
interfaces. Actual accepted maxima are listed above.

Sampling and topology invariants cannot prove absence of every geometric or UV
defect. Matched textured Blender renders of the cabin, cockpit and exterior were
also reviewed; no conspicuous new cracks, missing parts or silhouette changes
were observed at those views.

## Export and validation

- Staged GLB cooked successfully using the existing Release AssetCooker in an
  isolated package, producing 181 manifest entries. Its first-use log reports
  missing old cache blobs/manifest before writing them; the final cooker result
  reports `succeeded` and the output manifest exists.
- The published GLB hash matches the validated staged GLB.
- Geometry checks compare oriented triangle multisets across material regrouping.
  All ten unreduced meshes, including the three gear meshes, match their original
  runtime geometry. Gear vertex joint/weight values, joint palettes and inverse
  bind matrices are unchanged.
- All 378 gear animation channels have byte-identical time/value samples.
  The rig still has 126 bones; five representative gear poses remain finite and
  the retracted and extended poses differ as expected.
- All original `.meta` GUID and child-key allocations are preserved. The new
  common material appends one child key; existing keys are not recycled.
- Texture files on disk are unchanged. The master still resolves its textures.
- No engine frame-time benchmark was performed. Triangle, primitive and file-size
  reductions are measured; an FPS gain is not asserted.

## Comparison renders

| View | Before | After |
|---|---|---|
| Cabin | [Before](../previews/sf50_material_interior_before_cabin.png) | [After](../previews/sf50_material_interior_after_cabin.png) |
| Cockpit | [Before](../previews/sf50_material_interior_before_cockpit.png) | [After](../previews/sf50_material_interior_after_cockpit.png) |
| Exterior | [Before](../previews/sf50_material_interior_before_exterior.png) | [After](../previews/sf50_material_interior_after_exterior.png) |

## Next: existing normal maps, then baking

The optimized source already contains 46 unique normal-image files connected
across 71 used materials. The glTF model path does not consume them. Connect the
existing maps first, including linear sampling, tangent frames, resource lifetime,
and raster/SSGI/RT agreement. Then bake only detail lost through further geometry
reduction, using the backed-up high-poly source and validated cages/UV coverage.
Normal maps cannot repair silhouettes or holes and do not reduce material splits
by themselves; upholstery/stitch atlases would be a separate material-reduction
step.
