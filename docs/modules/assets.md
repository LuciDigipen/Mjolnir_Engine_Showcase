# assets

The runtime half of the offline asset pipeline: the blob container, the manifest,
and a load-on-demand cache. It does not import, convert or parse any source
format — `tools/AssetCooker` does that at build time.

`mir::assets` · depends `mir::core` (public) · API `modules/assets/include/assets/` · maintainer Sumin Park · reviewed 2026-09-06

## Model

Assets are cooked at build time and never at runtime, so the runtime parses one
container format rather than one per source format.

`tools/AssetCooker` walks every configured source root, writes one blob per file
named by its id, and writes one binary manifest. `engine_cook_assets` wires that
into a target and copies the output next to the executable, so a moved build
folder still runs. Terrain is not cooked: recipes (`*.terrain.json`) and
release files (`.tindex`, `.tpack`, anything under `terrain/releases`) are
TerrainTool's, and the cooker skips them by the rule in
`terrain/TerrainAssetPath.h` (see [terrain.md](terrain.md)).

The manifest also records what each asset depends on, and a logical path for
the assets something refers to by name. The cooker reads the dependencies from
each payload as it writes the blob, so nothing at runtime has to decode a blob
to learn what else it needs. `Closure` lists an asset and everything it reaches,
optionally only through blobs of chosen type tags.

```cpp
if (AssetCache *cache = app.Assets())
{
    if (const AssetBlobView *blob = cache->Load(MakeAssetId("assets/foo.txt")))
        Use(AsStringView(*blob));
}
```

## API

| Type | Header | Role |
|---|---|---|
| `AssetId` | `assets/AssetId.h` | A 64-bit id, 0 is null. `MakeAssetId` hashes a string, for assets the cooker generates |
| `AssetMeta`, `MakeRandomGuid`, `MakeChildAssetId`, `IsAssetToolingFile` | `assets/AssetMeta.h` | The `<source>.meta` file: its guid and importer state, a random guid, a package part's id from its guid and a key, and the files scans skip |
| `SourceAssetIds` | `assets/SourceAssetIds.h` | The guids of the source files under a set of roots: reads every `.meta`, creates missing ones, fails on a duplicate guid, finds a file's id from its logical path |
| `AssetPathResult` | `assets/AssetPath.h` | `NormalizeAssetPath`, `MakeLogicalAssetPath`: the one spelling of a logical path, or an `AssetPathError` |
| `AssetBlobHeader` | `assets/AssetBlob.h` | 32 bytes: magic, format version, type tag, content version, size, content hash |
| `AssetBlobView` | `assets/AssetBlob.h` | Header plus a span over the payload; `AsStringView` for text |
| `AssetManifest` | `assets/AssetManifest.h` | id to type tag, content version, cooked file, logical path and direct dependencies, over a shared path table and edge table. `FindByPath`, `Dependencies`, `Closure` |
| `AssetCache` | `assets/AssetCache.h` | `Load`, `Get`, `Invalidate`, `ResolvedPath` |
| `AssetsModule` | `assets/AssetsModule.h` | The manifest and cache, loaded at startup, and the reload candidate. `ReloadEpoch` counts committed reloads |
| `AsyncAssetService`, `AssetRequest`, `AssetLoadState` | `assets/AsyncAssetService.h` | Reads and decodes assets on a background lane, `Request`, `State`, `Acquire`, `DrainCompletions`, `CancelAll`, `Idle` |
| `AssetDecoder`, `AssetDependencies` | `assets/AssetDecoder.h` | A decode function with the type tags of the dependencies it reads, and the lookup it gets over the blobs already read |
| `AssetResolver` | `assets/AssetResolver.h` | `Resolve` turns a path inside the game's assets folder into an `AssetId` through the manifest, `ResolveFull` does the same for an engine asset's full path, `PathOf` gives the path for a message |
| `AssetSettings` | `assets/AssetSettings.h` | The `assets` section of `project_settings.json`, `loadThreads` |
| `TextureAsset`, `TextureMip`, `SurfaceMaterialAsset` | `assets/ModelAsset.h` | Typed texture v2 (format, color space, level 0 and higher mip descriptors) and the generic `.material.json` surface material, distinct from the glTF `MaterialAsset` |
| `ResolveAssetIdRoot` | `assets/AssetIdRoot.h` | Fills a `GameProject`'s asset id root; fails setup on an unusable `assetsRoot` |

`AssetsModule` loads the manifest in its constructor: a missing one is an empty
manifest, a corrupt one fails setup. `Cache()` is the same object for the
module's life, so a holder keeps its pointer across a reload. A reload is
`BeginReload` (a manifest it cannot parse throws), then holders prepare from
`Candidate()`, then `CommitReload` swaps it in or `AbortReload` drops it.

`AsyncAssetService` reads, parses and decodes assets on a background lane of the
app's `JobExecutor` and hands the results to the main thread. `Request` takes an
id and an `AssetDecoder<T>`, a decode function plus the type tags of the
dependencies it reads. The manifest is read on the main thread when a request is
made, the root and the dependencies of those types are copied into one job, and
the worker reads each file, checks its header against its manifest entry and
calls the decoder. A result is a `shared_ptr<const T>`, so a holder keeps its
data past `CancelAll`. `DrainCompletions` runs once a frame before the systems
update, and it is the only place a request changes state. The cache stays as it
was, and nothing moves onto the service until a consumer asks for it.

`assets.loadThreads` in `project_settings.json` is how many loads run at once,
2 by default and 1 to 32. A value outside that range keeps the default and logs
a warning, and the job queue lowers it to the executor's worker count.

## Rules

**An `AssetId` is the guid in the asset's `.meta` file.** The cooker writes
`<source>.meta` beside every file it cooks into an asset, with a random 64-bit guid
and, for a glTF, the part keys it has handed out. Commit it: a machine that cooks
without it would invent another guid and every stored id would stop matching. The
parts of a package, such as a glTF's meshes and clips, take their ids from the
package's guid and a key (`MakeChildAssetId`). Generated particle scripts are named
by a hash of their content, and a Prefab by its own UUID.

Cook roots must never be absolute, and `engine_cook_assets` rejects one. The root
string is part of every logical path, and a path is how code finds an asset, so an
absolute path would bake one machine's checkout location into names that have to
match on every machine.

**Every logical path goes through `assets/AssetPath.h`.** The engine and the cooker
build `<id root>/<relative path>` with `MakeLogicalAssetPath`, where the id root
is `games/<Game>/<assetsRoot>` (or `assets` for the engine) and the relative
path is what a scene stores. The rules are purely lexical: `\` becomes `/`,
empty and `.` segments drop, `..` pops a segment. A `..` that climbs out of
its own root, an absolute path, a drive letter, `:` or NUL is an error, not a
guess.

**Renaming or moving a source file keeps its id.** The `.meta` travels with the
file: the content browser moves it on a rename and removes it on a delete, it moves
a single file renamed outside the editor, and the cooker moves it with a model. A
scene that names an asset by path (`meshName`, `systemName`, `scriptName`,
`modelName`) still has to be rewritten after a rename, because a path is a name and
not an id.

**Never copy a `.meta`.** A copied `.meta` repeats the original's guid, so the cook
fails and names both files. Delete the copy's `.meta` and cook again. A `.meta` whose
source file is gone is ignored with a warning.

**Scans skip tooling files.** `IsAssetToolingFile` is true for `.meta`, `.import.json`,
`.pending` and `.gitkeep`. The content browser, the import manager and the cooker
leave them out, so a sidecar never shows as an asset and writing one never starts a
reimport.

**Code at runtime finds an asset by path through `AssetResolver`.** It normalizes the
path the way the cooker does and looks it up in the manifest, so a path
that no asset has is a null id and a reason, not a made-up id that fails later
inside the cache. It reads the manifest the module holds now, so a lookup after a
reload sees the new cook. `App` owns one, and a module takes it by reference
instead of the id root string. The resolver keeps a reference to `AssetsModule`,
which must outlive it. `Resolve` returns a null id for a path that is not valid
or that no asset has, and its optional `error` says which, so a caller checks
for null before it loads. `PathOf` gives the id in hex for an asset with no
path. The cooker, the particle source parsers and the
editor's file tools still build ids from source paths, because the files they
name may not be cooked yet.

**Case is part of the path.** `Terrain/A` and `terrain/A` are two paths even on
Windows, where both open the same file. Folding case would let two files collide on
a case-sensitive filesystem; a wrong case fails as a missing asset.


**`AssetCache` is move-only.** The `AssetBlobView` it hands out aliases the
entry's own buffer, so copying the cache would leave two views onto one buffer.

**A dependency edge always points at an entry of the same manifest.** `Parse`
rejects one that does not, and the cooker fails the cook naming the asset that
holds it, so a walk over the graph never meets a missing target.

**A logical path names one entry.** `Parse` rejects a duplicate. Sub-assets that
nothing refers to by name, such as a glTF's meshes, have an empty one, and an
empty name matches nothing.

**The service is main thread only, and a worker touches nothing shared.** A job
holds copied paths, type tags and versions, so it never reads the manifest or
the cache, and a reload cannot free anything it uses. A decoder runs on a worker,
so it must not touch shared state.

**Two requests for one asset and decoder share one load.** The key is the id and
the descriptor's address, so a descriptor has static storage, and the same
asset under another decoder is a separate load.

**The service remembers a failed load too.** It logs the failure once at the
drain, and asking again returns the same failed handle without reading anything,
until `CancelAll` or a reload.

**`CancelAll` and a committed reload make every earlier handle `Stale`.** The
service sees a reload through `ReloadEpoch` at its next drain or request. Loads
that have not started are dropped, running ones finish and their results are
discarded.

**With no workers a load runs inside `Request`.** Its result still waits for the
next drain, so a state changes at the same point in both modes.

**A failed load is remembered.** The cache does not re-check the disk every frame
for something that was not there.

**`App::Assets()` returns `nullptr` when there is no manifest.** Handle it rather
than asserting on it. A caller loses an asset, not the process.

**A cooked blob counts as current only if its mtime beats the source and its
header matches the current format.** An mtime test on its own would skip every
file after a format-version bump, while the manifest recorded the new version.

**Bump `contentVersion` whenever a payload layout changes.** That bump is what
forces a recook instead of a silent mismatch at load. `TextureAsset` is at
version 2 because it now carries format, color space and every mip above 0.

**A material's source images are not standalone assets.** A `.material.json`
packs its six image roles into three cooked textures. The full cook leaves the
source images out, and a partial material cook drops an older standalone
manifest entry for them.

## State

*As of 2026-10-26.* Ids, the blob container, the manifest and the cache all work.

`.gltf` and `.glb` cook to typed model payloads, see
[gltf-importer.md](../gltf-importer.md). A standalone `.png`, `.jpg` or `.jpeg`
cooks to a `TextureAsset` v2, decoded to RGBA8 with a complete mip chain, under
the id of its own path; an image a glTF references is left to that model's
import and stays a raw blob. A `.material.json` cooks to a
`SurfaceMaterialAsset` plus three packed `TextureAsset`s, see
[material-pipeline.md](../material-pipeline.md). `.particle.json` presets use the
versioned [particle effect schema](particles.md),
including a validated typed texture dependency. Other files without a dedicated
importer cook as raw passthrough blobs.

Tests in `modules/assets/tests/`: `AssetBlob`, `AssetCache`, `AssetManifest`,
`AssetMeta`, `AssetPath`, `AssetResolver`, `AssetSettings`, `AsyncAssetService`, `ModelAsset`, `SourceAssetIds`. The cooker side of path identity is checked in
`tools/AssetCooker/tests/GltfImporter.tests.cpp` ("Cooker ids match the engine's
logical paths for any root spelling").

## Backlog

1. **Typed importers.** A mesh, texture or config should cook into a layout the
   runtime can use directly rather than a raw byte copy. That needs a type tag
   and content version per kind, and extension recognition in the cooker.
2. **Hot reload from the cache side.** `Invalidate` exists and nothing watches.
   `script`'s `ScriptReloadWatcher` polls mtimes itself, because there is no
   shared watch infrastructure to build on.
3. **A packed archive** for distribution. Better to ship, worse to iterate, and
   it can be added later without changing any ids.
4. **Scenes that name an asset by path.** `meshName`, `systemName`, `scriptName`
   and `modelName` stay readable, so a rename means rewriting the scenes that use
   it. Guid fields with a picker would fix that, and the choice waits until there
   are real scenes. Emitter and effect source JSON also still names its texture and
   emitters by path, which the cooker turns into guids through the `.meta` files.

## Changed

*2026-10-31* — **Ids come from `.meta` guids.** Was: an id was a hash of the source
path, a glTF kept its identity in `<source>.import.json` with an alias for every path
it had been moved to, and renaming any other file made it a new asset. Now: every
cooked source file has a `<source>.meta` holding a random guid, the guid is the id,
a glTF's parts take theirs from the guid and a key, and the aliases and the old
sidecar are gone. A duplicate guid fails the cook, and every scan skips the sidecars.
Because a rename should not orphan the scenes, prefabs and particle sources that
hold an id. `AssetCooker --migrate-ids` moved the existing data: it dry-runs unless
given `--apply`, gives each glTF with an `.import.json` a guid, rewrites the old
ids under each `--rewrite-dir` and deletes the old file, and it can be deleted once
every game has migrated.

*2026-10-31* — **`AssetResolver` replaces the id root in the runtime modules.** Was:
`App` passed the id root string to the particles, animation, script and renderer
modules, and each one joined it to a path and hashed the result, so an unknown
path became an id that failed later inside the cache. Now: `App` owns one
`AssetResolver`, those modules and the game shell take it by reference, and a
path that no asset has is a null id with a reason. Because every one of those
lookups would otherwise change when ids stop being path hashes. The editor's
file tools and the particle source parsers are unchanged.

*2026-10-31* — **`AsyncAssetService`.** Was: `AssetCache::Load` read, parsed and
decoded on the calling thread, so a scene that opened blocked until every asset
it named was in memory, and the cache was not safe to hand to a worker. Now: the
service does that work on a background lane from copied manifest data and
publishes results at a drain, `LoadModelResources` has a form over blobs already
read, and the lane size is the `assets.loadThreads` setting. Because loading had
to stop blocking the frame without making the cache thread safe. The cache and
its callers are unchanged.

*2026-10-31* — **Manifest v2: logical paths and dependency edges.** Was: an entry
held an id, a type tag, a content version and the cooked file name, and the
cooker decoded emitter blobs again after writing them to find which generated
scripts to keep. Now: an entry also holds a logical path and a range into an
edge table, the cooker records the edges when it emits each blob, a script stays
only while an edge points at it, and a cook fails when an edge target is not in
the manifest. An old manifest is rejected, so the first cook after the change is
a full one. Because a request for a model needs its meshes and skeletons known
before any blob is read, and a lookup by name needs the manifest to carry the
name.

*2026-10-26* — **Surface materials and cooked mip chains.** Was: a texture was
RGBA8 level 0 only and the runtime generated mips. Now: `TextureAsset` v2
serializes format, color space and every mip level, and a `.material.json`
cooks a generic `SurfaceMaterialAsset` plus packed albedo/normal/surface
textures. Because the surface frontend needs deterministic packing and the GPU
must receive the authored chain unchanged. See
[material-pipeline.md](../material-pipeline.md).

*2026-10-24* — **`AssetsModule` and `ResolveAssetIdRoot`.** Was: `App` held the
manifest and cache, rebuilt the cache object on every import, and `app`'s
`GameProject` computed the asset id root. Now: the module owns both, keeps the
cache's address across a reload, and holds the candidate between prepare and
commit; `assets` fills the id root. Because: holders kept stale pointers unless
`App` told each one, and `project` cannot depend on `assets`.

*2026-10-02* — **Terrain files are no longer cooked (#116).** Was: a terrain
recipe under an assetsRoot became a raw blob like any other file. Now:
AssetCooker skips `*.terrain.json` (any case), `.tindex`, `.tpack` and anything
under `terrain/releases`, and `engine_cook_assets` leaves them out of the
cook's dependencies, so a terrain id is never also a raw asset's and editing a
recipe does not rerun the cook. No change to this module.

*2026-10-02* — **A standalone image cooks to a `TextureAsset`.** Was: every
`.png` and `.jpg` outside a glTF import cooked as a raw blob, so the runtime
would have had to decode it. Now: `AssetCooker` decodes it with the glTF
importer's decoder, now `tools/AssetCooker/ImageImporter.h`, and writes
`kTextureAssetTypeTag`. Its id is unchanged, and a corrupt image fails the
cook. Because `gameui` draws UI textures straight from the cache and reads no
file itself. See [gameui.md](gameui.md).

*2026-09-30* — **One normalizer for logical paths (#110).** Was: about eight
call sites in `app`, `animation` and `AssetCooker` joined root and relative path
their own way (`lexically_normal`, or a bare `"/"` concatenation), and none
rejected `..`. Now: all of them call `MakeLogicalAssetPath`, and AssetCooker
refuses an absolute or escaping `--source`/`--id-root`. Because the terrain
tool and the engine must derive the same id from one logical path.

*2026-09-18* — **The byte primitives left this module.**
`assets/AssetFileIo.h` is gone; its functions are in `core/fs/FileIO.h`, and
[core.md](core.md) carries that entry. Nothing about the blob or manifest formats
changed.

`AssetManifest` now exports `kAssetManifestFileName`. `app` and `AssetCooker`
each spelled `"manifest.blob"` independently, so a rename in one would have left
the other reading a file that is not there.
