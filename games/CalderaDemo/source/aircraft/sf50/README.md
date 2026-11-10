Cirrus SF50 — Arrivée Stellar / Family
====================================

Open `cirrus_sf-50.blend`. Keep `sf50_textures/` beside it; used images have relative paths.

Textures are organized as `sf50_textures/<component>/<surface>_<map>.png`, matching the mesh component names. Maps used by multiple components are in `shared/`. Map suffixes describe their purpose: `base_color`, `normal_opengl`, and `occlusion_roughness_metallic`. Blender image names match these readable component/surface names. `reports/sf50_texture_manifest.json` lists each file's materials, object usage, color space, original name, and SHA-256 hash.

The file contains one aircraft collection with 16 named mesh components and one landing-gear armature. Microsoft's Arrivee Stellar paint and family seat attachments are preserved.

Only the landing gear is rigged. The three gear assemblies include their bay doors, linkages, and flexible cables. Select `Landing gear rig` and scrub the `Landing gear | Retract - Extend` action: frame 1 is retracted and frame 111 is extended. The file opens with gear extended. All 13 other mesh components are static, including the fuselage, wings, cabin, and seats.

The gear has been reduced from 1,129,884 to 138,758 triangles and from 692,923 to 117,897 vertices. Dense mechanical surfaces are simplified; low-polygon panels and layered decals retain their original topology. The 126-bone rig, action, materials, UVs, and static aircraft remain. Five sampled gear poses passed bidirectional surface-distance checks (sampled maximum below 4.5 mm; 99.9th percentile below 1.5 mm). `reports/sf50_gear_optimization_report.json` records the measurements and local full-detail backup path.

The aircraft is unpowered. Source empty hierarchies, unrelated rigs/actions, hidden simulator variants, collision/helper meshes, probes, cameras, and inspection lights have been removed. Earlier versions remain recoverable under `builds/sf50_backups/` in this workspace.

Cleanup merges identical image content with matching color-space settings and identical material graphs, preserves PBR textures and normals, restores paint clear coat, and replaces simulator glass/vertex-color combinations with thin-sheet glass using Fresnel reflections.

`reports/` contains the current geometry validation, optimization measurements, texture manifest, and Microsoft source metadata. Historical preparation scripts and reports are archived locally under the ignored `builds/sf50_backups/cleanup_archive/` directory. `previews/` contains inspection renders; cameras and lights used for them are not saved in the blend. Local backups and scratch files are not needed to load the source or runtime model.

The Blender asset does not execute Microsoft Flight Simulator behavior scripts. Dynamic avionics, interactive controls, rain/weather shaders, gameplay animation bindings, and engine LOD/export integration require runtime work. The shared tablet and mount are outside the supplied Microsoft aircraft folder. Existing source normal conversion uses 8-bit PNGs derived from the supplied KTX2 maps.

The supplied Microsoft originals are read only and are not required to load this completed asset. Maintained tools are in `tools/sf50/` at the repository root. See that directory's README for validation, export, and placement commands. The Blender source and PNGs use Git LFS; Blender backup/autosave files are ignored.
