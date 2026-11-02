# animation

Model resource validation, clip sampling, pose evaluation, and per-instance
playback data, plus `ModelSystem`, which turns them into GPU draws. The module
does not own the renderer, the ECS update loop, or import of source glTF files.

`mir::animation` · depends `mir::assets`, `mir::core`, `mir::renderer` (public) · API `modules/animation/include/animation/` · review pending

## Model

`AssetCooker` turns a glTF package into a Model plus typed Mesh, Skeleton,
AnimationClip, Material, and Texture assets. `LoadModelResources` resolves the
Model's mesh, skeleton, clip and material dependencies through `AssetCache`, then
checks their cross-references before a model can be evaluated.
`DecodeModelResources` does the same over blobs already read, and
`ModelResourcesDecoder` names the mesh, skeleton, clip and material types it
needs, so a background load reads those and leaves textures to their own
requests. A material that is missing or does not parse is absent from the
result, and its primitives take the error appearance. The source format is never
parsed here.

`ModelPlayer` is the ECS component for one instance. It stores the model and
animation references, clip and scene selection, and the playback clock. The
cooked assets are shared and immutable; `EvaluateModel` builds a fresh node
pose from rest values for each sample, so seeking or evaluating two instances
does not mutate shared data. The pose follows stable node keys and the Model's
parent hierarchy. A `ModelMorphOverrides` world resource can replace sampled
weights for one entity and node without changing the asset.

`ModelSystem` is registered by `AnimationModule` in `LateUpdate` with the asset
service and the Vulkan renderer. The module also clears its per-entity state from
the `ModelPlayer` OnRemove hook, frees its models at `SceneUnload`, and starts
replacement preparation at `ReloadCommit`. It resolves each `ModelPlayer`, advances its clock only while Play is
simulating, evaluates a pose, and submits per-draw deformation data. The
renderer shader applies morph, then skin, then the instance transform;
`DeformPrimitive` provides the CPU reference for that order.

## API

Model updates gather inputs on the main thread, evaluate independent instances
through the optional app-owned job executor, then join and commit before Render.
Small batches and editor previews evaluate inline. Normal launches use an
automatic pool of up to four workers; `--workers 0` explicitly evaluates inline.
See [job ownership and configuration](jobs.md).

| Type or function | Header | Role |
|---|---|---|
| `ModelPlayer`, `RegisterModelPlayer`, `kModelPlayerFields` | `animation/ModelPlayer.h` | Serializable per-entity selection and playback state behind validating setters |
| `ModelSystem` | `animation/ModelSystem.h` | GPU upload, reload swap, playback clock and `ModelDrawItems` per frame |
| `ModelMorphOverrides` | `animation/ModelPlayer.h` | Transient weights keyed by entity and stable node key |
| `ModelResources`, `LoadModelResources`, `DecodeModelResources`, `ModelResourcesDecoder` | `animation/ModelAnimation.h` | Typed model and validated dependencies, from the cache or from blobs already read |
| `ModelEvaluationInput`, `ModelEvaluationResult`, `EvaluateModelInstance` | `animation/ModelEvaluation.h` | Immutable evaluation inputs and owned per-instance output, independent of World and renderer |
| `ModelEvaluationGrain` | `animation/ModelEvaluation.h` | Bounded grain targeting four ranges per participating thread |
| `SampleAnimationTrack`, `EvaluateModel`, `NodePose` | `animation/ModelAnimation.h` | Track interpolation and a fresh hierarchical pose |
| `ResolvePoseWorld`, `SkinMatrices`, `VisibleModelNodes` | `animation/ModelAnimation.h` | World transforms, skin palette, and selected glTF scene |
| `DeformPrimitive`, `BuildDeformationData` | `animation/ModelAnimation.h` | CPU reference and shader buffer encoding |

See
[glTF asset pipeline](../gltf-importer.md) for importer identity and editor use.

## Rules

**Keep instance state separate from cooked assets.** `ModelPlayer` contains
the time, speed, playing and loop flags. Sampling starts from rest pose on
every call. This makes seeking deterministic and prevents one entity's morph
weights or animation time from affecting another entity.

**Use stable asset IDs after resolution.** `modelName` is a path relative to the
game's asset root and is useful in hand-authored scenes. `modelAsset` and
`animationAsset` store resolved hex IDs; when set, they take precedence over
the path and clip index. The runtime fills missing IDs after a successful
load, so subsequent scene saves retain references across source moves and
clip reordering. A missing referenced clip stops playback with a warning.
The Inspector provides a Model picker and hides these raw reference fields;
`clip` and `scene` remain editable.

**The playback clock is a Play-mode concern.** `ModelSystem` advances time only
when the world is simulating and `playing` is true. Edit mode and Play pause
hold the clock. A fixed pose can be authored with `playing = false` and an
explicit `time`. There is no serialized preview flag. Older scene files with
`preview` load without using that key, and the next save omits it.

**State is private; the field table goes through setters.** `ModelPlayer`
exposes accessors only. Scene load, scripts and the Inspector write through
`kModelPlayerFields`, whose setters keep `time` and `speed` finite, `clip` at
least 0, `scene` at least -1, and parse asset references as 16-digit hex (any
other text becomes null). Asset references are held as `AssetId`, not text.

**Scene serialization follows the component field table.**
`RegisterModelPlayer` supplies the fields consumed by both scene save/load and
generic scripting. Removing a field from that table changes what scene files
persist; hiding it in the Inspector alone does not.

**Validate dependencies before drawing.** A clip track must target a model
node with a compatible channel, a skin palette must match its skeleton, and
morph weights must match the mesh. `LoadModelResources` rejects mismatches;
it does not silently render a partial model.

**A broken texture or material draws magenta; a broken mesh fails the model.**
`ModelPreparation` builds a model's meshes and materials once its CPU records
decode and every base color texture has settled. A material or texture blob that
is missing or does not parse logs an error with the model and asset IDs. The
primitive then gets the renderer's `ErrorTexture()`. A `CreateMaterial` that
returns null logs an error, and the primitive gets `ErrorMaterial()`. In every
case the primitive's tint becomes magenta too, because the ray tracer reads
only the tint. A failed texture logs once per preparation and is retried on the next
reload. A cooked `TextureAsset`
supplies its own format, color space and complete mip chain, so `CreateTexture`
uploads that chain instead of generating one. A failed `CreateMesh` still fails
the whole model, since there is nothing to draw.

**A model is prepared in the background and published whole.** `ModelSystem`
claims each referenced model from the App's `ModelLibrary`, which runs a
`ModelPreparation` for it, and that claims the
model's CPU records and one `TextureLoadLease` per base color texture, and never
waits on either. When every texture is ready or failed it claims the renderer's
shared material records, queues the primitives through the shared mesh upload
path, and publishes an immutable `PreparedModel` once the meshes are ready, so no
entity patches a material later. The first load keeps the model hidden until
then, and a structural failure or a failed mesh upload marks the model failed. A reload prepares a replacement per
loaded model from the new catalog while the old `PreparedModel` keeps drawing,
then switches that model when its replacement is usable. A replacement that
fails leaves the old model in place and logs a warning, and a failed texture
publishes the magenta error appearance like an initial load. Playback, poses
and deformations stay with the entity. A `PreparedModel` only holds claims on
renderer-owned records. `AnimationModule` owns the one library and the editor hands it to
`PrefabWorkspace`, so the scene and every prefab preview take theirs from it. The
library keys an entry by the model and the content of everything it names, its
records and the textures its materials name, so the scene and every prefab preview
that show a model hold one entry and one prepared model, and a World keeps only its playback clock, poses, morph
overrides and deformations. A reload claims the entry for the new content, which is
the same entry when nothing changed and another one that replaces it when it is
ready. The last World to drop an entry frees the prepared model, and the renderer
keeps the unused records under its residency target. A failed entry is prepared
again by the next claim. A World drops its claim on a
model when no entity of it names the model any more, so a deleted or renamed model
does not stay protected from eviction until the scene closes.

A model is a set of records, not one decoded blob. The model, each mesh, each
skeleton, each clip and each material is decoded by its own request, so two models
that name one mesh hold one decoded `MeshRecord` and one converted upload, and the
asset service shares it while anything holds it. The worker that decodes a mesh
converts its vertices for upload, so the main thread only queues them. Each record
reports every allocation it owns to the asset service, the morph target arrays and
the skeleton and node tables included, and a shared allocation is counted in the
record that owns it once. `ModelResources` is the assembled view a preparation
validates and a World evaluates from.

**A preparation belongs to the catalog it was requested under.** It keeps that
catalog and the content of everything the model names, and names its mesh and
texture records from it. When a reload would give any of them other content, the
preparation stops as superseded and publishes nothing, so decoded data from one
catalog never gets another catalog's key. The claim a reload takes for the new
content is a preparation of its own. A reload that changes nothing the model names
does not stop it.

## State

*As of 2026-09-24.* The module samples translation, rotation, scale, and
morph-weight tracks with STEP, LINEAR, and CUBICSPLINE interpolation. It
evaluates hierarchy, selected glTF scene visibility, skin matrices, and
optional per-instance morph overrides. `ModelSystem` shares one prepared model between the instances of a World, and
a reload prepares replacements in the background while the old models draw. Tests in `modules/animation/tests/ModelAnimation.tests.cpp` cover
interpolation, deterministic seeking, hierarchy, morph-before-skin behavior,
and invalid typed assets. GPU draw behavior is outside this unit test target.

There is no animator graph, blending, retargeting, root motion, or animation
preview window. The Inspector edits the existing playback fields and selects
a model, but it has no timeline or named clip picker.

## Backlog

1. Add an animator layer for state transitions and blending when gameplay
   needs more than one selected clip per instance.
2. Design a dedicated animation preview window with a transient clock and
   selection state. Keep preview state out of scene JSON.
3. Add a clip picker and timeline authoring controls to replace manual clip
   indices and time edits.
4. Profile per-draw deformation buffer uploads before pooling or caching them.

## Changed

*2026-10-08* — **Texture and material failures fall back to magenta.** Was:
a missing or unreadable texture or material blob, or a failed GPU texture or
material, threw out of `LoadInto`. The entity was marked failed and the model
did not draw at all. Now: each of those logs an error and the primitive draws
with the renderer's error texture or error material and a magenta tint. Only
a failed mesh still fails the model. Because: issue #19.

*2026-11-01* — **A reload reuses what did not change.** Was: every reload
prepared each model from nothing. Now: the decoded model is adopted by content, and
the mesh, texture and material records are found by their keys, so an identical cook
republishes the same handles and a cook that changes one mesh uploads that mesh
alone.

*2026-11-01* — **Models prepare in the background.** `ModelSystem` loaded a
model and uploaded its textures synchronously, and a reload staged GPU resources
at `ReloadPrepare` so a rejected cook vetoed the swap. `ModelPreparation` now
claims shared CPU records, texture leases and renderer-owned mesh and material
records, the module no longer stages or
aborts a model reload, and the replacement policy above covers late failures.
`ModelResources` also carries the materials the meshes name.

*2026-09-24* — **ModelSystem moved into this module; ModelPlayer made private.**
`ModelSystem` was in `app`, which left the component and its only system in
different modules; `animation` now links `renderer` publicly for it.
`ModelPlayer` became a class with accessors after review found every field was
writable from anywhere, including resolved asset IDs. Scene JSON is unchanged.

*2026-09-23* — **Preview removed from scene state.** `ModelPlayer.preview`
previously let Edit mode advance the playback clock and was saved by the
generic field table. Playback now advances only while simulating; a future
preview window will own its own transient state.
