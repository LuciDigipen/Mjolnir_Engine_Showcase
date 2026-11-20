# editor

Prefab preview Worlds retain their mesh and model resources across main-scene
unload. Closing a document releases only its claims and resources. The former
global renderer cleanup and preview resolve reset are not part of scene unload.

The Slate editor shell — dockspace, default panel layout, the File, Edit, Project
and Debug menus — drawn into the ImGui context `imguiruntime` owns. It owns no
part of that context's lifetime: no `CreateContext`, no fonts, no backend. See
[imguiruntime.md](imguiruntime.md). Undo and redo have their own page,
[editor-history.md](editor-history.md).

`mir::editor` · depends `mir::imguiruntime`, `mir::renderer`, `mir::window` (public); `mir::core`, `mir::editortheme`, `imgui`, `imguizmo`, `iconfontcppheaders` (private) · API `modules/editor/include/editor/` · maintainer Sumin Park · reviewed 2026-09-17

## Content browser previews

The content browser toolbar switches between List (the default) and Grid. List
uses shared type icons and requests no texture previews. Grid shows texture
previews and shared icons for scripts, folders, models, audio and other files.
The Size slider sets grid images from 48 to 128 pixels. Images preserve their
aspect ratio, transparent areas show a checkerboard, and hovering shows the full
filename. Selection and file actions work in either view.

Options sets the thumbnail cache capacity, 256 by default and at most 256. Zero
turns texture previews off. View, size and capacity are local editor preferences,
not project settings. Only visible grid cells request previews, excess visible
cells use their type icon when capacity is exhausted.

`EditorModule` owns `ContentThumbnailCache`. A dedicated async decoder filters
cooked textures on workers to at most 128 pixels on the longest side, using
cooked mips when suitable. Images used by materials may have only packed derived
outputs, and external model images may be raw blobs. These files use a cancellable
editor background lane to decode their source image, sharing the background image
decoder. Their cache identity includes the import revision, so recooking refreshes
them without changing material outputs. The preview preserves cooked color space and filters alpha in
premultiplied form. Only the small output survives the decode. Browser work
admits at most one new decode per frame, two outstanding decodes, and two small
uploads per frame through the regular upload budget. New requests wait while the
scene asset service is busy. With `--workers 0`, the
asset service runs decoding inline.

Leaving view cancels unneeded decode leases and retires incomplete uploads.
Ready previews remain in an LRU until eviction. Thumbnail uploads leave 64 image
table slots free for scene allocations. When the table fills, scene allocations
reclaim the least recently used unused thumbnails, including those retained in
List. Visible previews stay pinned through the current UI frame. Texture loads
that cannot obtain a slot remain pending and retry without an asset reload.
Thumbnail bytes also participate in the shared GPU residency budget. Changed cooked
content invalidates old previews, and failed previews
are remembered until reload. No disk thumbnail cache is written. Texture and
ImGui descriptor retirement both respect frames in flight. Mesh/model rendering
and real-time previews are outside this feature.

## Model

Project > Particles opens the source stack editor for the current game's particle
assets. It uses particles source readers/writers and the regular AssetImportManager
cook/apply path, with no particles_gpu dependency. GPU modules, renderer settings,
curves, gradients and distance quality can be tuned in Edit, then saved and
recooked. The preview uses the production ParticleWorld API. Its lifetime and
resource policies are described in [particles](particles.md#authoring-and-quality).
Draft switches require saving or discarding edits. Resource limits are session
settings, apply them while no effects are alive. File watcher bookkeeping absorbs
explicit saves, and successful stale batches retain their pending apply until
queued cooking finishes. Failed cooking displays a diagnostic and leaves live
assets available. The editor module remains excluded from game-only builds.

`App` drives `Editor` through several calls a frame and reads state back.

```cpp
mImGui.BeginFrame();                                    // ImGuiRuntime: the NewFrame trio

if (windowVisible)
    Act(mEditor.DrawLayout(mDesc.mEnableEditorLayout,   // EditorActions
                           world.Resource<InputMap>(), world.Resource<RenderSettings>(),
                           world.Resource<EngineModeState>(), projectMenu, currentProject, world,
                           mAssetImport.View(), mRenderer, mContentBrowser.View()));

mRenderer.SetImGuiDrawData(mImGui.EndFrame());          // ImGuiRuntime: ImGui::Render, hand off draw data
mRenderSystem.SetViewportRect(0, mEditor.SceneViewportRect());

if (IsEditing(world))                                   // Play shows the World's own camera instead
{
    mEditor.UpdateCamera(dt);
    mRenderSystem.SetExternalCamera(0, mEditor.SceneCamera().Pose(), mEditor.SceneCamera().mLens);
}
```

`DrawLayout` returns `EditorActions`: the `SceneMenuAction` the File menu or the
Play/Pause/Stop toolbar picked this frame, and the directory of a game picked
under Project > Open Project and confirmed. The editor decides what was asked for
and `App` decides what it means, so this module never touches `SceneIO`,
`PlaySnapshot`, a `LoadedScene` or a `GameProject` — `EngineModeState` is
read-only here, just to draw the toolbar's current label. `HierarchyPanel` reads
the `World`'s entities (see below) to display and select them. It never creates,
destroys or duplicates one itself. Each of those also changes the `LoadedScene`,
and only `App` holds it. `Editor` turns the `EntityAction` the Hierarchy returns
into a command for its `History`, which reaches the scene through the
`ISceneOps` `App` passed to `Initialize` (see
[editor-history.md](editor-history.md)).

Project > Open Project lists the `ProjectMenuEntry` values `App` passes in — each
game's name, with its directory in the shortcut column — and ticks the one whose
directory is `currentProject`. Picking another opens a modal warning that the
editor restarts with that game and that unsaved scene changes will be lost. Only
its Open button puts the directory into `EditorActions::mOpenProject`; `App`
does the restart.

The tab bar starts with the Scene and adds one tab per open Prefab document.
`DrawLayout` takes both Worlds because they are separate: `activeWorld` is the
document being edited, `sceneWorld` is always the scene. The active document's
viewport uses that document's camera, and opening a Prefab focuses its root once.
`EditorActions::mDocument` asks to activate or close a tab, and
`mSaveActiveDocument` asks for Save on whichever tab is active. The editor never
opens or closes an actual document; `App` owns the preview Worlds and the
registry, and reports the tabs back through `EditorDocuments`.

A Prefab tab's entities author through `IAuthoredEntities`, not the World.
`MakeSetFieldsEditCommand`, `MakeAddComponentEditCommand` and
`MakeRemoveComponentEditCommand` choose a Prefab-document command for an
instance member and a World command for an ordinary entity, so the Inspector and
the gizmo need no branch of their own. The Inspector shows an inherited field
greyed, an override with Revert, and a changed instance member with Apply to
Prefab. The Hierarchy's Create Prefab turns a scene subtree into a new document,
and dropping a Prefab row from the Content Browser into the Hierarchy or the
scene viewport places a linked instance.

The Transform header's Inspector menu offers **Snap to Terrain** on the Scene
tab in Edit. `SnapToTerrainActionFor` disables it with a reason when there is no
terrain coverage or the active document is a Prefab page. A request fills the
bottom footprint from a `GroundQuery` probe, fits a plane, samples a second
tilted footprint, and then submits one `Snap to Terrain` position+rotation
command through `AuthoredEdit`. Sampling retries outside the panel's `Draw`;
the session cancels on timeout, selection/document/Play changes, or a Transform,
parent or terrain revision change. At the commit boundary it revalidates that
context and refuses to queue its command when this frame submitted another edit
or requested a scene, Play or document transition.

`DrawLayout(true)` builds the dockspace once. It splits into left, right and
bottom with `DockBuilderSplitNode`, keeps the centre undocked through
`ImGuiDockNodeFlags_PassthruCentralNode`, docks `Hierarchy`, `Inspector` and
`Output` into the splits, draws a Play/Pause/Stop toolbar strip and the menu bar,
and optionally the Style Editor and Project Settings windows. Last, it runs
`History::Flush`, the frame's one safe point for undoable changes.

Passing `false` skips all of that and resets `SceneViewportRect()` to zero
extent, which the renderer reads as "use the full swapchain". An ImGui context
still runs, so a caller can draw its own UI.

`EditorCamera` is a free-fly camera for the scene viewport — a double-precision
`WorldPosition` plus yaw/pitch, handed out as a `WorldPose`, not a `WorldTransform` (that type's matrix is resolver-only,
unconstructable outside the ECS hierarchy resolve, and this camera is
deliberately outside the `World`: it must survive a Play session's
snapshot/restore untouched). `Update` reads `Window::RawInput()` directly
rather than `InputMap`/`InputState`, so a project's key bindings can't affect
the editor's own camera. Right-drag flies/looks (WASD/QE, scroll for speed);
`Editor::UpdateCamera` gates that on `!WantsCaptureMouse()` so a drag ImGui
itself is handling doesn't also move it. A jump (`FocusOn`, `SetPosition`, a
document switch) marks a camera cut, which `EditorModule::FeedViewport` takes
with `TakeCut()` and hands to `RenderSystem::CutViewport(0)`, so the frame
after it reuses no temporal history; flying does not cut.

### The module

`EditorModule` owns the `Editor`, the asset import, the content browser, the
scene ops and the Prefab documents. Its ImGui drawer draws the layout, turns the frame's actions into
requests (scene commands, a game to open) or carries them out (an import, a
content browser action, opening a file in the IDE), then feeds viewport 0's rect
and, in Edit, the editor camera to `RenderSystem`, and the scene viewport to
the `GameUIArea` resource for the game UI drawn later that frame, with
`mToolsHavePointer` set in Edit while the gizmo, the view pie or an extension
that owns the viewport has the pointer. The drawer is
in the `Tools` group, which runs before the game UI's. Its `FrameBegin` work polls
the import and the content browser and asks for the swap once a cook finishes.
Its stage functions keep the history and the unsaved mark: `SceneLoad` clears
the history and sets the mark from `SceneModule::LastLoad()`, `SceneSaved`
clears it, `PlayBegin` remembers it for Stop to put back, and `ReloadCommit`
marks the import applied.

`AssetImportManager` caches the import panel's model view. Source additions,
edits and removals invalidate it through the existing 250 ms watcher poll;
that poll also checks the cooked import-cache file's timestamp/size signature
for external replacement, creation or deletion. Unchanged frames reuse the view
without rescanning sources or reparsing JSON. Explicit actions, cooker completion
and reload apply/failure still refresh immediately, so statuses and diagnostics
do not wait for the next poll. Layout-disabled editor builds keep these services.

The Prefab documents are a `PrefabWorkspace`, one preview World per open Prefab
tab, and a `PrefabAuthoringAdapter`. The adapter bridges History and the prefab
module's authored state; `prefab/PrefabAuthoring` owns the edits themselves.
Apply and Revert write no files. A `LateUpdate` system runs the open previews in
Edit, after `PrefabSystem` committed their documents, and the drawer hands the
active tab's preview to the panels and to `RenderSystem::SetSourceWorld`. Play
shows the Scene tab, and the `SceneLoad` after Stop reopens the tab it left, by
prefab id; a prefab deleted during Play leaves the Scene tab. `PlayBegin`
runs before the snapshot: `PrefabModule` commits the pending edits and freezes
the prefabs, then the editor keeps the Prefab tabs' unsaved marks for Stop to put
back (`PrefabPlayMarks`). A rename or rewrite of a Prefab file reaches the
coordinator through `ContentBrowserWatcher` and `PrefabModule::Rescan`, which waits
while the prefabs are frozen, and a document whose file changed under it loses its
save point. The browser also lists a new Prefab that has no file yet, and
`WorldSceneOps::PlacePrefab` places a Prefab by its content path through the
workspace. A tab whose Prefab vanished closes after the frame's layout. The previews' models take part in an asset swap, and their caches reset at
`SceneUnload`, after the renderer freed every upload.

Save on a Prefab tab writes that Prefab. On the Scene tab it first writes the
modified Prefab sources the scene uses, then asks for `SaveScene`, which `App`
runs at the top of the next frame. At `SceneSaved` the module plans the save
again, and a prefab that changed in between keeps the scene unsaved, so the next
Save writes both. Each Prefab write clears that document's
saved mark without clearing History. A missing source path or a failed write
stops the save and leaves the scene unwritten; files already written stay
saved.

## API

```cpp
bool Initialize(const ImGuiRuntime &imgui, Window &window, std::filesystem::path projectSettingsPath,
                World &world, ISceneOps &sceneOps, bool srgbOutput);    // false if imgui is not initialized
void Shutdown();                  // before ImGuiRuntime::Shutdown

void ClearHistory();              // App calls it on New, Load, Play and Stop
bool IsSceneUnsaved() const;      // the title bar's * marker
void SetSceneUnsaved(bool unsaved);  // false: the scene as it is now is the saved one
bool IsDocumentUnsaved(DocumentId document) const;              // the same for a Prefab or other document
void SetDocumentUnsaved(DocumentId document, bool unsaved);
void SetAuthoringBackend(IAuthoringBackend *backend);           // the module's PrefabAuthoringAdapter
void SetAuthoredEntities(IAuthoredEntities *authored);          // document edits and instance overrides
void SetAudio(IEditorAudio *audio);                             // null: no Audio settings, no soundtrack picker

// between ImGuiRuntime::BeginFrame and EndFrame
EditorActions DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
                         PostProcessingSettings &postProcessingSettings, const EngineModeState &mode,
                         std::span<const ProjectMenuEntry> projects, std::string_view currentProject,
                         World &activeWorld, World &sceneWorld, const EditorDocuments &documents,
                         const AssetImportView &assetImport, const VulkanRenderer &renderer,
                         const ContentBrowserView &contentBrowser, bool materialPanelVisible,
                         bool materialDraftsUnsaved, float dt);

VkRect2D SceneViewportRect() const;
bool IsInitialized() const;

void UpdateCamera(float dt);          // once per frame, after DrawLayout
EditorCamera &SceneCamera();
```

| Type | Header | Role |
|---|---|---|
| `EditorModule` | `editor/EditorModule.h` | The editor's module: its ImGui drawer, `FrameBegin` work and stage functions |
| `AssetImportManager`, `ContentBrowserWatcher` | `editor/AssetImportManager.h`, `editor/ContentBrowserWatcher.h` | The import queue and the cooker child; the content browser's disk poll, which hides `.meta` and other tooling files and moves a `.meta` with its file |
| `ShaderHotReload` | `editor/ShaderHotReload.h` | Development-only manifest/depfile watcher, direct-slangc batches, staged publication and revision cancellation; runs in Edit and Play |
| `WorldSceneOps` | `editor/WorldSceneOps.h` | The `ISceneOps` the history uses, on the World and its `LoadedScene` |
| `CreateGameProject` | `editor/CreateGameProject.h` | Writes a new game directory; the scene through `scene`'s own save |
| `SceneMenuAction` | `editor/Editor.h` | `None`/`New`/`Save`/`Load`/`Play`/`TogglePause`/`Stop` |
| `ProjectMenuEntry` | `editor/Editor.h` | `{mDir, mName}` for one game in Open Project |
| `EditorActions` | `editor/Editor.h` | `{mScene, mOpenProject, mCreateProjectName, mAssetImport, mContentBrowser, mDocument, mSaveActiveDocument, mToggleMaterialEditor}`, what the menus and toolbar asked for this frame |
| `EditorDocuments`, `EditorDocumentTab`, `EditorDocumentAction`, `EditorDocumentActionKind` | `editor/Editor.h` | The Scene tab plus every open Prefab tab; `Activate` and `Close` requests, and `SaveActiveDocument` |
| `IAuthoredEntities`, `AuthoredAction`, `AuthoredKey`, `AuthoredFieldState`, `AuthoredComponentState` | `editor/history/AuthoredView.h` | The editor's view of Prefab-instance overrides, without a Prefab type |
| `MakeDocumentEntityCommand` | `editor/history/DocumentNodeCommands.h` | Create, delete and reparent nodes of the active Prefab document as one command each |
| `MakeSetFieldsEditCommand`, `MakeAddComponentEditCommand`, `MakeRemoveComponentEditCommand`, `MakeCreatePrefabCommand`, `MakeRevertAuthoredCommand`, `MakeApplyAuthoredCommand` | `editor/history/AuthoredEdit.h` | Routes an Inspector, gizmo or Hierarchy edit to a Prefab document when the entity is an instance member |
| `ContentBrowserView`, `ContentBrowserEntryView` | `editor/panels/ContentBrowserPanel.h` | The open project's content root as a flat list of files/directories, plus `mLastRenamedFrom`/`mLastRenamedTo` for one tick when a same-tick remove+add looks like a rename |
| `ContentBrowserAction`, `ContentBrowserActionKind` | `editor/panels/ContentBrowserPanel.h` | `{mKind, mTargetPath, mNewName}`: `CreateFolder`, `Rename`, `Delete`, `OpenInIde` or `OpenPrefab`, for `App` to carry out (see [app.md](app.md)) |
| `kPrefabContentPayload` | `editor/panels/ContentBrowserPanel.h` | The drag payload a Prefab row carries into the Hierarchy or the scene viewport |
| `IEditorAudio`, `EditorAudioLevels`, `EditorAudioChannel`, `EditorSoundBank` | `editor/EditorAudio.h` | What the editor reads and changes of the game's audio: the live volumes, the project's defaults and each SoundBank's events. `App` implements it with the audio module and sets it on `EditorModule::SetAudio`, so the editor never links `audio` |
| `IdeSettings`, `DetectDefaultIde` | `editor/IdeSettings.h` | The `project_settings.json` "ide.path" string (empty: auto-detect VS Code), edited from `ProjectSettingsPanel`'s External Tools section |
| `EntityAction`, `EntityActionKind` | `editor/panels/HierarchyPanel.h` | `{mKind, mPreset, mTarget, mTargets, mParent, mBefore}`: `Create` (null `mPreset` = New), `Delete`, `Duplicate`, `CopyAsOrdinaryObject`, `Reparent`, `CreatePrefab` or `PlacePrefab`, which `MakeEntityCommand` or `MakeDocumentEntityCommand` turns into a command (see [editor-history.md](editor-history.md)) |
| `NeighborInHierarchy` | `editor/panels/HierarchyPanel.h` | The entity to select after deleting one: next sibling, else previous, else the parent |
| `IsBaselineComponent`, `AddMissingBaseline` | `editor/EntityBaseline.h` | The components every editor-created entity has: `Transform`, `WorldTransform`, `Name` |
| `EditorCamera` | `editor/EditorCamera.h` | Free-fly scene camera, `WorldPosition` + yaw/pitch (`Pose()`, `Translate`, `FocusOn`, `Cut`/`TakeCut`), outside the `World` |
| `widgets::*` | `editor/EditorWidgets.h` | The editor's UE5-style ImGui widgets: `CategoryHeader`, `BeginPropertyGrid`/`PropertyRow`/`EndPropertyGrid`, `AxisDragN` (X/Y/Z/W colour-tagged fields), `SearchBox`, `ToolbarButton`, `IconTextButton`, `PushRowSelectionColors`, `MatchesFilter`. Colours come from `EditorTheme` |
| `MaterialDraft`, `MaterialDraftStore`, `RefreshMaterialDraft`, `ResolveMaterialCook` | `editor/MaterialDraft.h` | Per-asset reflected values, accepted cooked baseline migration, dirty-only preview overlays and request-scoped cook state |
| `MaterialRecipe`, `SaveMaterialRecipe`, `MaterialSourceFile` | `editor/MaterialRecipe.h` | Reads an authored `.material.json` and writes only changed parameter and texture-path fields |
| `MaterialEditor` (private) | `src/editor/MaterialEditor.h` | The Material window, its reflected controls, target selection, cook feedback and Save/Revert wiring |
| `Selection` | `editor/Selection.h` | `{mSelected, mAnchor}` (a `vector<Entity>`, ordered by selection - `.back()` is primary - and the shift-click range start), the `World` resource `Hierarchy` and viewport picking write and `Inspector`/the gizmo read |

`LogPanel`, in `src/editor/panels/LogPanel.cpp`, renders `core`'s `LogHistory`
into the `Output` dock. `HierarchyPanel`, in
`src/editor/panels/HierarchyPanel.cpp`, renders the `World`'s live entities
into the `Hierarchy` dock as a tree built from `HierarchyIndex`, roots in
`Roots()` order. `InspectorPanel`, in
`src/editor/panels/InspectorPanel.cpp`, renders the `Selection`'s components
into the `Inspector` dock. None of the three are public — panels are reached
only through `DrawLayout`.

`ContentBrowserPanel`, in `src/editor/panels/ContentBrowserPanel.cpp`, is a
floating window (Project > Content Browser) rather than part of the fixed
dock split: a folder tree built client-side from `ContentBrowserView`'s flat
entry list on the left, and a file/folder table for the selected folder on
the right — directories are explicit entries, so an empty or just-created one
still shows up. Clicking selects, double-clicking a folder navigates into it,
and a row's context menu offers Rename and Delete; Delete goes through a
confirm modal (file changes are outside undo, which covers only scene edits -
see [app.md](app.md) for `ContentBrowserWatcher`, which actually watches the
disk and carries out the mutations). This closes #152: the panel never caches a stale listing, so
a file added, removed or renamed outside the editor shows up on the watcher's
next poll with no restart.

The Material window (Project > Material) follows a `.material.json` selected in
the Content Browser, or the selected entity's reflected `materialPath`. It
builds one control per reflected field, keeps one draft per normalized project
and logical asset path, previews only dirty numeric fields through
`SurfaceMaterialLibrary::PreviewParameters`, and saves only changed recipe
fields through `UpdateJsonFile`. Its accepted baseline is the cooked
`SurfaceMaterialAsset`, not raw source text. The cooked apply identity decides
when a cook exists, but the baseline moves only after
`SurfaceMaterialLibrary::Publication` reports that asset's current content as
`Published`; an unrelated asset, a preview or a shader commit cannot stand in
for it. A pending replacement, and a failed one with its diagnostic, keep the
previous accepted baseline and the dirty retry state, and an inactive material
settles without claiming a GPU publication. A first selection with no accepted
draft waits for publication or shows the failure instead of creating a clean
draft from unaccepted cooked bytes. A no-change retry after a renderer
publication failure requests the importer's forced reimport once per Save, so it
re-enters the real replacement path instead of reporting GPU success. A
shader-only field addition
fills the new control from the validated catalog or typed default and a deleted
field stays authored, reported as unsupported and preserved on save, so the
panel opens without a recook. A draft is previewed while any frontend of its
asset is active, and the published batch covers the mesh and terrain records of
that asset together. Ordinary Revert is
memory-only and queues the accepted numeric values until they are published. Play
disables the controls.

Clicking an entity in `Hierarchy`, or clicking it directly in the scene
viewport (`Editor::UpdateViewportPicking`, a ray/AABB test against every
`(WorldTransform, MeshRenderer, MeshBounds)` entity - see
[renderer.md](renderer.md) for `MeshBounds` - plus `RayIntersectsSphere`
against every component that registered a pick radius, such as `Light`; the
nearest entry along the ray wins; a click on a game UI widget, as the scene
World's `GameUIPointer` says, is the game UI's and picks nothing), writes it into `Selection`
(`editor/Selection.h`), a `World` resource `HierarchyPanel` installs on
first draw if nothing already has. Both call the same
`ApplyClick(Selection&, Entity, bool additive)`: a plain click replaces the
whole selection, `additive` toggles the clicked entity's membership instead,
and a null `Entity` (clicked empty space) clears the selection unless
`additive`. Either click sets `mAnchor`. The viewport passes `io.KeyCtrl ||
io.KeyShift` as `additive`, since it has no row order. `Hierarchy` passes
`io.KeyCtrl` alone, and sends shift-clicks to `ApplyRangeClick`, as Windows
Explorer does. Shift-click selects the visible rows from `mAnchor` through the
clicked row, and Ctrl+Shift adds them to the selection. Rows inside a
collapsed parent are not visible, so they are skipped. The clicked row ends up
primary and the anchor stays. The range is taken after every row is drawn, so
it sees rows below the clicked one. An anchor that is dead or hidden starts
the range at the click. `HierarchyPanel` prunes any no-longer-alive
entities out of `mSelected` every frame, so a stale handle never lingers
past the frame its entity died - this runs before viewport picking in
`DrawLayout`, so picking never has to prune again itself. `InspectorPanel`
reads it with `TryResource` rather than `Resource`, so it degrades to "No
entity selected" instead of asserting on a frame where `Hierarchy` hasn't
drawn yet (e.g. it starts collapsed); with more than one entity selected it
shows a plain "N objects selected" instead of drawing components. The
transform gizmo (`DrawViewportGizmo`) only ever manipulates
`Selection::Primary()` (the most recently selected entity) - there is no
multi-object gizmo, so the others stay selected but untouched by it.
`DrawSelectionHighlights` draws a wireframe box (from each entity's
`MeshBounds`, projected with the same `viewProj` `UpdateViewportPicking`
builds) over every selected entity, not just the primary - brighter/opaque
for the primary, dimmer for the rest - since the gizmo alone gives no
viewport-visible sign that a non-primary entity is still selected.

Right-clicking empty space in `Hierarchy` opens Create. Its first item, New,
creates an entity from no preset, and the items below it come from
`EntityPresetRegistry`. Right-clicking a row opens Duplicate and Delete for that
entity, and selects it first if it was not already selected. The Delete key and
Ctrl+D do the same for `Selection::Primary()`, while the panel has focus and no
text field is active. Each of these only fills `EntityAction`. `Editor` turns
it into a command and `History` runs it at the end of `DrawLayout` (see
[editor-history.md](editor-history.md)), so Ctrl+Z undoes it. The command
selects the result. After a delete it selects `NeighborInHierarchy`, computed
before the entity is gone.

Dragging a row reparents or reorders. The drag carries the whole selection, or
only the grabbed row if a Ctrl-press removed it from the selection. Each row
splits into three bands over its height plus half the gap on each side. The top
quarter inserts before the row and draws a line above it. The middle half makes
the row the new parent and draws a box around it. The bottom quarter inserts
after the row as its next sibling, as Godot does, even when the row is open.
Anywhere no row claims, the panel's whole area takes the drop, and the entities
become the last roots. The drag tooltip names what the hovered target would do,
such as "Make child of Player".

A press on a row that is already part of a multi-selection waits for the
release, as in Explorer and Unity. A release without a drag then collapses the
selection to that row, and a drag carries the whole selection instead.

The ReParent command puts the dragged list in hierarchy order and drops any
entity whose ancestor is also listed, so a subtree moves once and siblings keep
their order. It moves each through `SetParent`, which keeps the world pose, and
selects them. The whole drop is one step. A drop after the last sibling under the current parent is
`MoveToLastSibling`. On the next frame the panel opens their ancestors and
scrolls the last one into view, only as far as needed.

While dragging, the panel scrolls when the mouse is within 8 px of its top or
bottom edge, at 960 px/s scaled by frame time. A collapsed row hovered for 0.7 s
opens. Rows opened that way stay open after a drop and close again if the drag
is cancelled. These numbers are Unity's; Unity's 16 px per drag event is 960 px/s
at 60 fps.

`InspectorPanel` walks every `seq` up to `kMaxComponentTypes`, asking
`ComponentRegistry::FindBySeq` for each one and skipping those the selection
doesn't `Has` — the same walk `SceneIO` (see [scene.md](scene.md)) uses to
save a scene, and why it treats a C++ and a script-declared runtime component
identically: both surface through the same `ComponentOps`/`FieldDesc` tables.
Each field reads into a 64-byte scratch buffer (large enough for a `Mat4`,
`FieldKind`'s biggest kind), an ImGui control drawn over the copy per
`FieldKind`. Only when the control reports an edit does the new value go to
`History` as a Set fields command, which calls `FieldDesc::Write` at the end of
the frame — so a component with a custom accessor (`Transform`'s `SetPosition`
and friends) runs through it rather than being memcpy'd under it (see
[editor-history.md](editor-history.md)). A `FieldDesc::ReadOnly` field (`WorldTransform.matrix`) still shows its
current value, greyed out via `ImGui::BeginDisabled`. (A 3D transform gizmo
does exist, drawn over the viewport by `DrawViewportGizmo` via ImGuizmo - it's
a separate, `Selection`-driven overlay, not part of `InspectorPanel`'s
field-table drawing described here.)

A `FieldWidget::AssetRef` field with `AssetKind::SoundEvent`, the `Soundtrack`'s
and the `AudioEmitter`'s `eventName`, is a combo of the game's SoundBanks with
their events under them, read through `IEditorAudio::SoundBanks()` each time it
opens, so a bank regenerated while the editor runs shows up. A pick writes the
event and, into the field `mBankField` names, its bank, as one Set fields
command, so one undo takes back both; an emitter has no bank field. The
`Soundtrack`'s bank field is `mHidden` and shows in the combo's preview instead.
On a Prefab instance the event shows as overridden when either is, and its
Revert and Apply to take both overridden keys as one command through the span
overloads of `MakeRevertAuthoredCommand` and `MakeApplyAuthoredCommand`, refused
whole if either is refused, so an event is never left with another's bank.

A bank holds its events by ID only, so one without Wwise's `<bank>.json` beside
it, such as `HelloWorld`'s `Test`, is listed with its events unknown and a box
under it to type an event into; Enter picks it from that bank. Banks with their
JSON are pick-only. A current event no bank lists, typed earlier or into an
unknown bank, still shows at the top as "(not listed)".

**Audio in Project Settings.** With `IEditorAudio` set, the panel has an Audio
section between Physics layers and External Tools. In Edit its sliders change a
draft of the project's default music and SFX volume and mute. Save writes the
`audio` section of `project_settings.json`, applies the defaults only after the
write succeeds and makes them what the pause menu's Restore defaults goes back
to. The audio module applies the saved defaults when no per-user settings file
exists, and never rewrites a player file. "Reload Saved Audio
Defaults" reads the saved section back and shows the problem when the file is
unusable. In Play the defaults are greyed out and a "While playing" group changes
what is heard at once. `EditorModule` takes the volumes at `PlayBegin` and puts
them back at `PlayEnd`, so neither that group nor the pause menu's Audio screen
changes anything for good, and the defaults are never written from them. With
`--no-editor` the volumes are not put back, as in a game build.

**Project Settings, Post Processing and Input Settings apply on Save.** Their
widgets edit panel-local drafts of `RenderSettings`, `PostProcessingSettings`,
`InputMap`, `PhysicsLayerTable` and audio defaults. Opening a panel initializes
its draft from the current project values. A successful Save writes that section
to disk, then publishes the draft to the shared runtime value. A failed write
keeps the active value unchanged and retains the draft for retry. Closing the
panel discards unsaved edits, and reopening starts from the current active value.
External Tools likewise applies the saved IDE settings on the next launch.

SSGI, DDGI and layer names validate edits before accepting them into their drafts.
Invalid GI values disable Rendering Save. "Reload Saved Physics Layers" and
"Reload Saved Audio Defaults" apply a valid saved section and replace the draft
only on success. Physics layers and audio defaults stay greyed out in Play.
The explicitly live "While playing" audio controls still preview changes at once.

Component edits go to `History` as commands, like entity actions, so each one
can be undone. They change no `LoadedScene` entry, so `App` has nothing to do
for them. Once no ImGui item is active after an edit, the panel calls
`History::EndStep`, so one drag, one typed value or one click is one step. The
"+ Add" button at the right of the header row lists every registered component the
entity lacks, except the baseline, and picking one submits Add component. Below
it, Search Details filters by component or field name: a matching component name
shows all its fields, otherwise only the matching fields show. Each component is a
category bar over a label|value grid; every grid in the panel shares one column
splitter.
Right-clicking a component's header opens Copy Values, Paste Values and Remove
Component. Copy Values reads every field
through `FieldDesc::Read` into the panel's `Clipboard`. Paste Values is enabled
only on a component of the copied type. When the entity lacks that type, Add
Component offers "Paste <type>" at the top instead, which adds the component
and then pastes into it.

## Rules

**`SceneViewportRect()` comes from `DockBuilderGetCentralNode`, never from a
named window.** The central node is the undocked centre where the swapchain shows
through, so it is the real viewport even though it has no window and no name.

Reading a window's content region instead is the bug this rule exists to prevent.
See Changed.

Anyone adding a genuine named viewport window has to update `DrawLayout`'s
source. There is no compiler or runtime check for it.

**The rect is only valid within or after `DrawLayout(true)` in the same frame.**

**Viewport gizmos draw into the `##ViewportOverlay` window, not a foreground or
background draw list.** The window sits behind every other window, so panels,
menus and modals, dimming included, cover the transform gizmo, the selection
boxes and component gizmos. The foreground list paints over every window, and
the game UI paints over the background list. The window spans the main viewport
because ImGuizmo hit-tests its rect. Each draw clips itself to the scene viewport.

**A component gizmo's line is cut where it passes behind the scene camera, not
dropped.** `DrawWorldLine` clips the segment at a small positive clip-space `w`,
so a long line such as a selected `Camera`'s frustum still shows while the scene
camera stands inside it. `GizmoCanvas::Aspect` gives the drawer the scene
viewport's width over height, which is also the aspect Play renders at.

**The Open Project dialog is opened after the menu bar ends, not from the menu
item.** `OpenPopup` and `BeginPopupModal` have to run under the same ID stack.
Called from inside the menu, `OpenPopup` opens a popup that nothing ever draws.

**Initialize after `ImGuiRuntime`, shut down before it.** `Initialize` refuses an
uninitialised runtime. It turns on docking, applies the theme and its DPI scale,
and points `io.IniFilename` at `imgui.ini` next to the executable; ImGui reads
that file lazily on the first `NewFrame`, so setting it after `CreateContext`
still loads it.

**`Shutdown` saves `imgui.ini` and detaches it.** `DestroyContext` would otherwise
save through `IniFilename`, a pointer into this `Editor`'s string, after the
editor has shut down. It saves only if ImGui got as far as loading settings, the
same guard `DestroyContext` uses, so a failed startup never overwrites the file
with an empty one.

**Background textures outlive `Editor` and are freed from ImGui's shutdown hook.**
`Editor::Shutdown` runs before `ImGuiRuntime::Shutdown`, so the renderer backend
still owns the GPU textures when `BackgroundImage` is destroyed. Its destructor
therefore hands the active and retired CPU textures to an
`ImGuiContextHookType_Shutdown` hook with its own payload, rather than freeing
them early or retaining a dangling `BackgroundImage` pointer. The hook runs after
the backend destroys the GPU side, unregisters and deletes the CPU textures, and
logs if real renderer data somehow remains. Never reorder the editor before
`ImGuiModule` to "fix" this.

**`Editor` is a class rather than free functions**, because it carries state
across calls — the viewport rect, the Style Editor toggle, the game waiting on
the confirmation dialog, and whether it is initialised. As free functions those
would be out-parameters threaded through every call.

Fonts are Inter for body text and Font Awesome 6 for icons, merged into one atlas
by `imguiruntime`; `EditorTheme::ScaleForDpi` rescales them here for the window's
`ContentScale()`.

**The Play/Pause/Stop toolbar is another `Up` viewport side bar**, drawn right
after the title bar and before the dockspace, so it stacks the same way the
title bar already does — each `BeginViewportSideBar` call shrinks the main
viewport's remaining work area for whoever asks next.

**`EditorCamera` has no `Transform` component, deliberately.** It isn't a
`World` entity, so a `Transform` has nothing to attach to; `SetExternalCamera`
takes the camera's world matrix directly rather than a `WorldTransform`.

**The File menu's New/Save/Load are disabled during Play**
(`ImGui::BeginDisabled(mode.mMode == EngineMode::Play)`). This is the visible
half of the rule; `App` enforces it independently (see [app.md](app.md)) since
this module only greys out a button, it does not stop another caller from
reaching `App::SaveScene()` directly.

**Every entity the editor creates has `Transform`, `WorldTransform` and `Name`.**
The gizmo moves an entity through its `Transform`, and the Hierarchy labels a row
by its `Name`. `App` runs the preset first and `AddMissingBaseline` after it.
`AddComponent` keeps a component that already exists, so a preset's own `Name`
wins over the default `Entity<index>`. `IsBaselineComponent` keeps the three out
of Add Component and greys out their Remove Component.

**Paste writes field by field through `FieldDesc::Write`, never as raw bytes.**
Its Set fields command does the writing, and so do the gizmo's.
Copying `Transform`'s bytes would copy its version stamp too.
`WorldTransform` is rebuilt only when that stamp differs from the one it was
built from, so a pasted position could be skipped. Writing each field runs
`Transform`'s setters instead, and they bump the stamp.

Every writable field is pasted, hidden ones included. `ModelPlayer`'s hidden
fields are persisted data, so skipping them would paste half a component. A
`ReadOnly` field is skipped. An `EntityRef` keeps its target, so a pasted
reference points where the copied one did.

**Nothing the Inspector submits changes the `World` while it draws.** History
applies every command at the end of `DrawLayout`. Removing or adding a component
moves the entity to another archetype, which would invalidate the component
pointers the rest of the loop draws from, so it must not happen mid-loop.

**The gizmo's drag is one step.** `DrawViewportGizmo` submits the one channel
the operation moves (position, rotation or scale) every frame the gizmo
changes it, and calls `History::EndStep` on the frame `ImGuizmo::IsUsing()`
turns false.

**Entity and component edits are allowed during Play.** Stop restores the `World`
from `PlaySnapshot`, so it discards them along with everything else Play changed.

**A drop is checked by the same code that refuses it.** Every row target calls
`CanSetParentAll`, the check `SetParent` makes. One entity that cannot move
refuses the whole drop, and the ReParent command runs the same check again
before moving anything. An invalid row still takes the payload, so the root target behind it
does not light up. It draws nothing and shows the not-allowed cursor.

**Row targets accept with `AcceptBeforeDelivery`.** Without it,
`AcceptDragDropPayload` returns the payload only on the release frame, so the
band indicator would never draw while hovering. The indicator is drawn after
every row, because the next row's hover background would cover a line drawn
earlier.

**The root target is submitted after the rows.** ImGui gives an overlapping
payload to the smallest target that accepts it, so a row wins over the panel's
whole area. Each row's target is stretched by half the gap above and below, so a
drop between two rows never falls through to the root.

**The reveal runs one frame late.** `History` applies the action at the end of
`DrawLayout`, after `Draw` returns, so the panel keeps the dropped list and opens
the new ancestors on the next `Draw`, when the graph has changed.

## State

*As of 2026-09-14.* One fixed layout: `Hierarchy` left, `Inspector` right,
`Output` bottom, a Play/Pause/Stop toolbar strip, and the scene showing through
the passthru centre. It is built once and left alone, persisted only through
ImGui's own `imgui.ini` next to the executable, which also carries it across an
Open Project restart.

`Output` is wired to `LogHistory` through `LogPanel`. Its history starts empty in
the process an Open Project restart launches.

`EditorCamera` flies and looks in Edit mode; `App` stops feeding it input and
switches viewport 0 to the World's default camera in Play (see
[app.md](app.md)).

*As of 2026-10-08.* `Hierarchy` creates, duplicates and deletes entities, and
`Inspector` adds, removes, copies and pastes components. Each acts on one
entity: the right-clicked row, or `Selection::Primary()`.

*As of 2026-10-09.* `Hierarchy` reparents and reorders by drag and drop, for
the whole selection, and the order survives save and Play.

*As of 2026-10-19.* Create, Delete, Duplicate and reparenting in `Hierarchy`
can be undone and redone, with Ctrl+Z, Ctrl+Y or the Edit menu (see
[editor-history.md](editor-history.md)).

*As of 2026-10-20.* So can every `Inspector` edit and the gizmo's move, rotate
and scale. No panel writes the scene's `World` directly any more. The title bar
shows `*` after the game's title while the scene differs from its last save, with
a tooltip saying so; the save point lives in `History` (see
[editor-history.md](editor-history.md)).

*As of 2026-10-23.* The document bar shows the Scene plus one tab per open
Prefab, each with its own camera and unsaved marker. `Hierarchy` offers Create
Prefab and a Prefab drag target, `ContentBrowser` opens a Prefab on double-click
and drags one into the scene, and `Inspector` reverts or applies instance
overrides through `IAuthoredEntities`. Node create, delete and reparent in a
Prefab document are undoable commands. Authored commands, previews, references and saves are covered by `modules/editor/tests/`; smoke tests cover the full engine path.

The panels and menus have no tests. They need a device and a display.
`smoke.switch` covers the restart that Open Project asks for, but not the menu
or the dialog themselves. History needs neither, and its tests are in
`modules/editor/tests/`.

## Backlog

1. **No multi-object transform gizmo.** With more than one entity selected,
   `DrawViewportGizmo` only manipulates `Selection::Primary()` - moving
   several selected objects together as a group, preserving their relative
   offsets, needs a shared-pivot gizmo this doesn't have.
2. **Serialized custom layouts**, beyond whatever `imgui.ini` remembers.
3. **Input routing**, beyond what `ImGui_ImplGlfw` gives for free.
4. **Make `Editor` optional on `App`** — a `unique_ptr`, null in a packaged game
   build. This module's boundary is what makes that swap possible without
   touching `RenderSystem` or `VulkanRenderer`.
5. **A second viewport showing `EditorCamera` during Play+Paused**, so pausing
   lets you look around the live World independent of the game camera. Needs
   `RenderSystem::AddViewport` (see [renderer.md](renderer.md)) wired to a
   second panel; nothing calls it yet.
6. **Viewport picking only tests axis-aligned bounding boxes**, not actual
   mesh geometry - a click inside an object's box but over empty space
   between its triangles (e.g. near a corner) still selects it. No
   per-triangle picking, matching the renderer's own choice not to retain
   CPU-side mesh geometry after upload.
7. **Delete and Duplicate act on one entity, not the whole selection.**
8. **`ContentBrowserPanel` has no drag-drop, multi-select, copy/paste or
   thumbnails** — icons are Font Awesome glyphs by extension, since no
   texture-thumbnail infrastructure exists in this module. Renaming or
   deleting an asset does not rewrite scene references to it (see
   [assets.md](assets.md)'s `AssetId` rename gap); Delete's reference guard
   (in `App`, see [app.md](app.md)) only checks the top-level path being
   deleted, not everything recursively under a deleted folder.

## Changed

*2026-11-20* — **Viewport picking leaves game UI widgets alone.** Was: a click on a game button in Edit also selected the entity behind it. Now: picking skips a click the game UI holds, and the drawer reports the gizmo, the view pie and a viewport-owning extension in `GameUIArea::mToolsHavePointer` so the game UI stays out of their way. Because: one click did two things. See [gameui.md](gameui.md).

*2026-11-05*, **The settings panels edit the project's values.** Was: `ProjectSettingsPanel`, `PostProcessingPanel` and `InputSettingsPanel` copied the live value into a whole-settings draft, assigned it back on Save, and so hid a script's later write and overwrote it. Now: the widgets edit the shared value, Save only writes it, SSGI, DDGI and a layer name edit a buffer until valid, the audio defaults edit through `SetDefaults`, Save no longer sets the live levels, and "Revert" became "Reload Saved Audio Defaults" and "Reload Saved Physics Layers". Because: a draft is a second owner of project configuration, and an invalid value in the renderer or the physics table would otherwise be applied or logged for every keystroke.

*2026-10-28* — **A reflected Material window with per-asset drafts.** Was:
shared surface materials could only be changed by editing `.material.json` by
hand. Now: Project > Material follows the Content Browser's material selection
or an entity's reflected `materialPath`, builds controls from the compiled
reflection, previews dirty numeric values in the shared raster/RT record, and
keeps a per-project/asset draft whose accepted baseline is the cooked asset.
Save writes only changed fields and queues a finite material cook; Revert is
memory-only; Play is read-only. Schema changes migrate compatible dirty fields
and report removed/retyped ones. Project open, New Project, window close and
quit ask before discarding unsaved drafts.

*2026-10-28* — **Audio: a soundtrack event picker and an Audio section in
Project Settings (#305).** Was: a `Soundtrack`'s event and bank were typed by
hand, and the default volumes could only be set by editing
`project_settings.json`. Now: the event is picked from the SoundBanks' events,
which sets the bank too, and Project Settings saves the default music and SFX
volume and mute and changes the live ones in Play, put back on Stop. Because:
designers set a scene's soundtrack and the project's volumes without editing
files, and a trial in Play should not leave the volumes changed.

*2026-10-27* — **Switching the active camera is one undo step;
`MakeCommandGroup` (#231).** Was: the Inspector recorded only the camera it
turned on, and `ActiveCameraSystem` cleared the old one outside history, so
undo left no camera active. Now: the Inspector's field edits and Paste Values
go through `MakeFieldEditCommand` (`editor/history/ActiveCameraEdit.h`), which
groups turning a `Camera` on with clearing every other active one, using
`MakeCommandGroup` (`editor/history/CommandGroup.h`): several commands done in
order and undone in reverse as one step. Because: undo should bring back the
camera that was active.

*2026-10-27* — **The scene camera has no depth of field or motion blur
(#231).** Was: the Post-Processing panel's focus, aperture and shutter applied
to the scene view as to every camera. Now: those values and exposure are
`Camera` fields read from the viewing lens, and `EditorCamera::mLens` sets
aperture and shutter to 0, with exposure 1. Because: a game camera's focus
distance and shutter mean nothing from wherever the scene camera is flying.

*2026-10-27* — **Gizmo lines clip at the scene camera; `GizmoCanvas::Aspect`
(#231).** Was: a gizmo line with either end behind the scene camera was not
drawn, and a drawer could not size anything to the viewport. Now: the line is
cut where it crosses behind the camera, and the canvas reports the viewport's
aspect. Because: a `Camera`'s frustum runs from its near plane to a far plane
100 m out, so standing inside it hid it.

*2026-10-26* — **A modal covers the viewport gizmos.** Was: the transform
gizmo, the selection boxes and component gizmos drew into the main viewport's
foreground draw list, which ImGui paints after every window, so they showed
through a modal and its dimming, and the gizmo could still be hovered and
dragged through the backdrop. Now: they draw into `##ViewportOverlay` (see
Rules). While `GetTopMostPopupModal()` is set, `ImGuizmo::Enable` is off, which
also greys the gizmo, and W, E and R leave the operation alone. Floating panels
and open menus over the viewport cover the gizmos too.

*2026-10-26* — **A light no longer takes viewport clicks across its whole
range.** Was: a Point or Spot light's pick sphere had the light's `range` as
its radius (an Area light's was 1.5 × `areaSize`), and the light was ranked by
its center's distance along the ray, so a click on empty space or on an object
behind the light's center anywhere inside that range selected the light. Now:
every light picks with a 0.3 m sphere at its origin, ranked by where the ray
enters it (`RayIntersectsSphere`, see [renderer.md](renderer.md)), the same
measure the mesh boxes use.

*2026-10-25*, **A Physics layers section in `ProjectSettingsPanel`.** Was: the panel drew Input, Rendering and
External Tools, and every body collided with every other. Now: it edits the `physicsLayers` section of
`project_settings.json` (up to 32 layer names and a collision grid), which the physics module reads, and a saved
table is applied to the running `PhysicsWorld` while editing. Because: a game needs to say which layers pass
through each other, and the settings window is where the other project-wide choices live. The table type and its
file are in the plain `physics_layers` library, which the editor links instead of `physics`; the panel edits the
shared `PhysicsLayerTable`, which physics compares with its applied copy between steps.

*2026-10-24* — **`EditorModule`; the import, content browser, scene ops, Prefab
documents and project creation moved in from `app`.** Was: `App` held the
`Editor`, the `AssetImportManager`, the `ContentBrowserWatcher`, `AppSceneOps`,
the `PrefabWorkspace` and the `PrefabAuthoringAdapter`, applied the editor's
actions, and cleared the history and set the unsaved mark at each transition.
Now: the module does all of it through its drawer, its `FrameBegin` work, a
`LateUpdate` system and stage functions, and `CreateGameProject` writes the default scene
through `scene`. Because: `App` no longer knows what the editor keeps, and the
scene file had a hand-written copy of `scene`'s format.

*2026-10-22* — **Theme preference; `Initialize` takes `srgbOutput`.**
`EditorPreferences::theme` (`"theme": "midnight" | "slate"`) picks the editor
theme, set from Preferences and applied at the start of the next frame.
`Editor::Initialize` gained `bool srgbOutput` (App passes
`VulkanRenderer::SwapchainIsSrgb()`) so the theme shows its colours as written.

*2026-10-22* — **UE5-style panels.** Was: Inspector fields drew as ImGui's
value-then-label widgets under collapsing headers, Add Component sat at the bottom,
and the Hierarchy was a bare tree. Now: the Inspector is laid out like UE5's
Details panel (header row with "+ Add", Search Details, category bars,
label|value grids, X/Y/Z-tagged vector fields), the Hierarchy has a search box
that keeps the matches' ancestors, an "Item Label" header and its own scrolling
row area, the gizmo toolbar is icons, and Play/Stop carry coloured icons. The
shared pieces live in `editor/EditorWidgets.h`. Because: the editor moved to a
UE5-like look (see [editortheme.md](editortheme.md)).

*2026-10-21* — **`PostProcessingPanel::Draw` takes a `PostEffectContext`
(PR #278).** Was: every effect's params UI drew the same regardless of
pipeline or hardware. Now: `Editor` passes `{ RayTracingActive(rayTracing),
DlssAvailable() }` through to each effect's draw callback, so the
anti-aliasing entry's Mode combo lists only the modes this device can run
right now - the same flags `RenderSystem` clamps the mode against (see
[renderer.md](renderer.md)'s anti-aliasing rules). Because: a combo offering
Dlss on a machine that can't run it would save a mode that silently renders
as Off.

*2026-10-21* — **History knows about documents other than the scene (#243).**
`Editor` gained `IsDocumentUnsaved`, `SetDocumentUnsaved` and
`SetAuthoringBackend`, which pass through to `History`. The title bar still
shows the scene's marker only. See [editor-history.md](editor-history.md).

*2026-10-20* — **The title bar marks an unsaved scene (#206).** Was: nothing
showed whether the scene had changed since it was saved. Now: `DrawTitleBar`
adds `*` after the title while `History::IsUnsaved()`, and `Editor` exposes
`IsSceneUnsaved`/`SetSceneUnsaved` for `App` to set the save point.

*2026-10-20* — **Inspector and gizmo edits go through undo history (#204).**
Was: `InspectorPanel` wrote fields, pasted, added and removed components on the
`World` itself, and `DrawViewportGizmo` called `Transform`'s setters. Remove
Component waited until after the component loop for that reason. Now: both
submit commands to `History`, `InspectorPanel::Draw` takes the `History`, and
each drag is one step. See [editor-history.md](editor-history.md).

*2026-10-19* — **Entity operations go through undo history.** Was: `DrawLayout`
returned the Hierarchy's `EntityAction` in `EditorActions::mEntity`, and
`App::ApplyEntityAction` carried it out on the spot. Nothing could be undone.
Now: `Editor` owns a `History`, turns the action into a command with
`MakeEntityCommand`, and runs it at the end of `DrawLayout`. `Initialize` takes
the `ISceneOps` `App` implements, and an Edit menu sits between File and
Project. See [editor-history.md](editor-history.md).

*2026-10-18* — **A standalone Post-Processing panel (#207).** Was: no place
for post-effect controls other than Project Settings. Now: `PostProcessingPanel`
(`editor/panels/PostProcessingPanel.h`), opened from Project >
Post-Processing, is a first-class panel shaped like `InputSettingsPanel` -
not another `ProjectSettingsPanel` subsection - with the same draft-copy/Save
pattern: edits only touch a draft until Save writes it to
`project_settings.json` and assigns it onto the live `PostProcessingSettings`
World resource, so every effect applies next frame with no restart.
`DrawLayout` gained a `PostProcessingSettings &` parameter, threaded through
from `App` the same way `RenderSettings &` is. It lists whatever
`PostEffectRegistry` (see [renderer.md](renderer.md)) has registered - a
checkbox per effect, its own params UI indented underneath when enabled -
and currently reads "No post effects yet", since this issue registers none.

*2026-10-18* — **The Create menu gained a `Primitive` submenu: cube, sphere,
plane, cylinder, capsule, cone.** No `HierarchyPanel` change needed - these
are just six more `EntityPresetRegistry` entries, registered under
`"Primitive/*"` names from `RegisterRendererEntityTypes` (renderer,
`ComponentRegistration.cpp`), the same grouping-by-slash-prefix `Light/*`
already uses. Each spawns a `Transform` + `MeshRenderer` (named via one of
`Shapes.h`'s reserved builtin names, `materialShader` "default") + `Name`;
`MeshResolveSystem` generates its geometry without an asset file (see
[app.md](app.md)). Because: issue #214.

*2026-10-10* — **Double-clicking a `.lua` or `.json` file in the Content
Browser opens the whole project folder as the workspace root, in a new IDE
window, with the file open.** Was (2026-10-09): opened just the file, no
project root. `IdeSettings` (`editor/IdeSettings.h`) gained `argsTemplate`
("ide.args" in `project_settings.json`, default `kDefaultIdeArgsTemplate` =
`"--new-window {project} {file}"`), edited next to "ide.path" in
`ProjectSettingsPanel`'s "External Tools" section. `IdeSettings::BuildArgs`
splits the template on whitespace and substitutes `{project}` (the game's
root dir) and `{file}` (the double-clicked file's absolute path) into each
token - no shell quoting involved, so flags/placeholders can't contain
spaces. The default targets VS Code's CLI; pointing `ide.path` at a
different tool means also rewriting `ide.args` to that tool's syntax.
`ContentBrowserPanel`'s `IsTextFile` (was `IsScriptFile`, `.lua`-only) gates
which files trigger this - an explicit allowlist, not everything-but-known-
binary, so new binary asset types stay excluded without a code change there.

*2026-10-09* — **Double-clicking a `.lua` file in the Content Browser opens
it in an external IDE.** New `IdeSettings` (`editor/IdeSettings.h`): the
"ide.path" string in `project_settings.json`, edited from a new "External
Tools" section in `ProjectSettingsPanel`. Empty (the default) falls back to
`DetectDefaultIde()`, which looks for a VS Code install at the usual
per-user/system locations. `ContentBrowserPanel::Draw` reports this as
`ContentBrowserActionKind::OpenInIde`; `App::ApplyEditorActions` resolves the
configured (or detected) IDE and calls `core`'s `LaunchProcess` with the
file's absolute path, rather than routing it through `ContentBrowserWatcher`
(see [app.md](app.md)) - it spawns a process, it doesn't touch the project
tree.

*2026-10-09* — **Added `ContentBrowserPanel`.** A folder tree + file table for
the open project's content root, closing #152 (disk changes made outside the
editor - add, remove, rename - now appear without an editor restart). Backed
by `app`'s `ContentBrowserWatcher` (see [app.md](app.md)), which polls the
project's content root the same way `AssetImportManager` polls its asset
subtree, rather than widening that manager's cook-queue-specific watcher to
every file type.

*2026-10-09* — **Shift-click in `Hierarchy` selects a range.** Was: shift-click
toggled one entity, the same as ctrl-click. Now: it selects the visible rows
from the anchor, and Ctrl+Shift adds the range. The viewport keeps shift as
ctrl, since it has no row order.

*2026-10-09* — **`Hierarchy` reparents and reorders by drag and drop.** Was: the
tree could not be edited, and roots were drawn in slot order. Now: rows are drag
sources and drop targets, `EntityAction` has a `Reparent` kind, and a press on a
selected row waits for release so a drag can carry the selection.

*2026-10-08* — **`Hierarchy` creates, duplicates and deletes entities, and
`Inspector` adds, removes, copies and pastes components.** Was: the Create menu
called `World::CreateEntity` and ran the preset inside `HierarchyPanel`. That
entity never entered the `LoadedScene`, so Save dropped it (#96). `Inspector`
only edited the fields of components already present. Now: `HierarchyPanel`
returns an `EntityAction` through `EditorActions`, and `App` creates through
`CreateSceneEntity`, so every created entity is saved. Delete and Duplicate take
the same route. Component add, remove, copy and paste stay inside
`InspectorPanel`, because they change no `LoadedScene` entry.

*2026-10-02* — **Optional.** Was: built and linked into every binary. Now: not
built at all when `MJOLNIR_WITH_EDITOR` is OFF (the `windows-msvc-game-*`
presets), along with `editortheme` and ImGuizmo; `app` is the only module that
links it, and only with the option on. See [app.md](app.md#without-the-editor).

*2026-09-30* — **The scene camera has no far plane.** Was: `EditorCamera::mLens`
kept the default `farZ` of 100 m, so the scene view clipped everything beyond
it. Now: its `farZ` is 0, which the reverse-Z projection reads as no far plane
(see [renderer.md](renderer.md)). ImGuizmo still needs a finite one, so
`DrawViewportGizmo` gives it a plane a few times farther than the entity.

*2026-09-29* — **The scene camera, picking, highlights and gizmo became
large-world safe; F frames the selection.** Was: `EditorCamera::mPosition` was a
`glm::vec3` and `Matrix()` a float world matrix; picking, the selection
outlines and ImGuizmo all narrowed entities against the Scene origin, so at the
map edge everything snapped to float's 1/64 m grid. Now: the camera keeps a
`WorldPosition` and exposes `Pose()`; those three paths work in the scene
viewport's camera-relative space (`TryMatrixRelativeTo(camera.position)`,
`MakeCameraRay`, `RayIntersectsBox`). The gizmo writes back one channel per
operation: Translate turns `camera + offset` into a root position or, through
`TryWorldToParentLocal`, a child's parent-local translation; Rotate and Scale
never touch the position. `FocusSelection` (F, Edit only, not while typing)
moves the camera next to `Selection::Primary()` with `EditorCamera::FocusOn`.

*2026-09-26* — **The ImGui context's lifetime moved to `imguiruntime`.** Was:
`Initialize` created the context, loaded the fonts and initialised both
backends, `Shutdown(VulkanRenderer&)` tore them down, and `BeginFrame`/`EndFrame`
were the `NewFrame` trio and `Render`. Now: all of that is `ImGuiRuntime`'s,
`Initialize` takes the runtime instead of the renderer, `Shutdown` takes nothing,
and `BeginFrame`/`EndFrame` are gone. A game UI can draw into the same context
without an editor in the build.

*2026-09-21* — **Shift-click is now equivalent to ctrl-click, and every
selected entity gets a viewport outline, not just the primary.** Was:
`additive` was `io.KeyCtrl` alone, so shift-click silently behaved like a
plain replacing click; the only viewport-visible sign of selection was the
gizmo on `Selection::Primary()`, so ctrl-clicking a second entity looked
indistinguishable from replacing the selection unless you were watching the
Hierarchy panel. Now: `additive` is `io.KeyCtrl || io.KeyShift` in both
`Editor::UpdateViewportPicking` and `HierarchyPanel`, and a new
`Editor::DrawSelectionHighlights` draws a wireframe `MeshBounds` box over
every entity in `Selection::mSelected` each frame (before
`DrawViewportGizmo`, same `viewProj` `UpdateViewportPicking` builds, via
`ImGui::GetForegroundDrawList` the same way the gizmo already draws over
the scene).

*2026-09-21* — **Clicking an object in the scene viewport selects it, and
`Selection` supports more than one entity.** Was: the only way to select
anything was a `Hierarchy` row, and `Selection::mSelected` was a single
`Entity`. Now: `Editor::UpdateViewportPicking` runs each frame (after
`HierarchyPanel::Draw`, before `DrawViewportGizmo`, in `DrawLayout`),
edge-detecting a left click inside the scene viewport rect (guarded by
`!WantsCaptureMouse()` and `!ImGuizmo::IsOver()/IsUsing()`, so it never
fires over a panel or while dragging the gizmo), builds a world-space ray
from the click (`CameraMath.h`'s new `MakeCameraRay`, using the same
`viewProj` convention `MakeViewProjection` already produces), and finds the
nearest entity whose `MeshBounds` (new component, see
[renderer.md](renderer.md)) it hits in local space
(`RayIntersectsAABB`). `Selection::mSelected` is now a `vector<Entity>`;
`ApplyClick` (`editor/Selection.h`) is the one place the
replace/toggle/deselect rule lives, shared by `HierarchyPanel` and viewport
picking so ctrl-click behaves identically from either entry point.
`InspectorPanel` shows "N objects selected" for more than one;
`DrawViewportGizmo` manipulates only `Selection::Primary()`.

*2026-09-17* — **`Inspector` populated from the Selection.** Was: an empty
`ImGui::Begin`/`End`, same as `Hierarchy` before it. Now: `InspectorPanel`
reads `Selection` (see above) and, for each component the selected entity
`Has`, draws its `FieldDesc` table as ImGui controls — `DragFloat`/`Vec3`/
`Vec4`/etc. per `FieldKind`, a read-only field shown greyed out rather than
skipped, edits written back through `FieldDesc::Write` so a component with an
accessor (`Transform`) runs its setter instead of being overwritten byte for
byte. Walking `ComponentRegistry::FindBySeq` rather than something typed to
C++ components is what makes a script-declared runtime component show up the
same way, with no separate code path.

*2026-09-17* — **`Hierarchy` populated from the World.** Was: `Hierarchy` and
`Inspector` were both `ImGui::Begin`/`End` with nothing drawn between them.
Now: `DrawLayout` takes a `World&`, and `HierarchyPanel` walks it every frame
— roots are entities `HierarchyIndex::ParentOf` reports null for, children
come from `ChildrenOf`, so the tree is read fresh each frame rather than
cached, which is what makes it track entities a running scene creates or
destroys without any extra invalidation. Clicking a row writes the entity into
the new `Selection` resource; `Inspector` (issue #38) reads it from there.
`Inspector` itself is still an empty window.

*2026-09-17* — **Project Settings gained a Rendering section.** Was:
`ProjectSettingsPanel` drew only the Input section (actions/bindings), and its
own "Save" wrote `InputMap` to `project_settings.json`. Now: it also takes a
`RenderSettings&` and draws a Rendering section below Input — a ray tracing
checkbox today, laid out as one row per setting so the next one is a row, not
a rewrite — with its own "Save" writing `RenderSettings` (see
[renderer.md](renderer.md)) to the same file. Each section's `SaveFile` reads
the file first and only replaces its own top-level key, so saving one section
never touches the other's. `DrawLayout` threads the `RenderSettings&` through
to `ProjectSettingsPanel::Draw` the same way `InputMap&` already was.

*2026-09-14* — **Project > Open Project.** Was: `DrawLayout` returned a bare
`SceneMenuAction`, and the Project menu held only Project Settings. Now: it
returns `EditorActions`, which adds a game directory picked from the Open Project
submenu and confirmed in a modal. `App` passes the list in and restarts into the
picked game, so this module still knows nothing about games.

*2026-09-12* — **File menu greyed out during Play.** Was: New/Save/Load stayed
clickable regardless of mode, and `App` (see [app.md](app.md)) silently
no-oped them with a log warning. Now: `ImGui::BeginDisabled` around the three
items while `mode.mMode == EngineMode::Play`, so the UI itself shows why
nothing happens rather than relying on the log.

*2026-09-12* — **`EditorCamera` and a Play/Pause/Stop toolbar added.** Was:
`DrawLayout` took only `enableLayout`, and the scene viewport always showed
whatever World `Camera` `RenderSystem` found first. Now: `DrawLayout` also
takes `InputMap`/`EngineModeState`, draws a toolbar that reads/writes Play mode
through `SceneMenuAction`, and `Editor` owns a free-fly `EditorCamera` that
`App` feeds into viewport 0 while editing.

Driven by wanting the scene view navigable independent of any World camera,
and by Play/Pause needing a visible way to start and stop, not just the
`EngineModeState` resource existing.

*2026-09-03* — **The module was extracted from `App.cpp`.** Was: about 150 lines
of `ImGui::*` and `ImGui_Impl*` calls sitting next to window and ECS
orchestration, with `App.h` needing dockspace internals just to declare a member.
Now: this module, with a six-call surface.

*2026-09-03* — **The scene rect stopped coming from a window named `"Scene"`.**
Was: read from `ImGui::Begin("Scene")`'s content region, which is the natural
read of the name. Now: read only from the dockspace central node, and the
left-docked window is named `Hierarchy` so that no name implies it is the
viewport.

The original compiled, linked and ran with no warning. The rect was internally
consistent and correctly clamped, and it clipped the scene to a 300x650 sidebar
nowhere near the geometry, so the cube rendered into nothing with nothing to
point at.
