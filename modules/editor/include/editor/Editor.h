/**
 * @file Editor.h
 * @author Sumin Park
 * @brief The Slate editor shell (dockspace, Hierarchy/Inspector/Output
 *        panels, Debug menu), drawn into the context ImGuiRuntime owns.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/EngineMode.h>
#include <core/ecs/Entity.h>
#include <core/ecs/components/Transform.h>
#include <core/platform/Surface.h>
#include <editor/BackgroundImage.h>
#include <editor/BuildSettings.h>
#include <core/ecs/Requests.h>
#include <project/GameProject.h>
#include <editor/EditorCamera.h>
#include <editor/EditorExtension.h>
#include <editor/EditorPreferences.h>
#include <editor/SnapToTerrain.h>
#include <editor/ViewPie.h>
#include <editor/history/History.h>
#include <editor/panels/AssetImportPanel.h>
#include <editor/panels/BuildSettingsPanel.h>
#include <editor/panels/ContentBrowserPanel.h>
#include <editor/panels/HierarchyPanel.h>
#include <editor/panels/InputSettingsPanel.h>
#include <editor/panels/InspectorPanel.h>
#include <editor/panels/LogPanel.h>
#include <editor/panels/PostProcessingPanel.h>
#include <editor/panels/PreferencesPanel.h>
#include <editor/panels/ProfilerPanel.h>
#include <editor/panels/ProjectSettingsPanel.h>
#include <imguiruntime/ImGuiRuntime.h>
#include <renderer/DebugView.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <glm/vec3.hpp>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <string_view>
#include <vector>

namespace mir
{
    class IAuthoredEntities;

    enum class SceneMenuAction
    {
        None,
        New,
        Save,
        Load,
        Play,
        TogglePause,
        Stop,
    };

    enum class GizmoOperation
    {
        Translate,
        Rotate,
        Scale,
    };

    enum class GizmoSpace
    {
        Local,
        World,
    };

    struct ProjectMenuEntry
    {
        std::string mDir;
        std::string mName;

        // why it cannot be opened, shown on hover; empty when it can
        std::string mUnavailable;
    };

    enum class EditorDocumentActionKind
    {
        None,
        Activate,
        Close,
    };

    struct EditorDocumentAction
    {
        EditorDocumentActionKind mKind = EditorDocumentActionKind::None;
        DocumentId mDocument = kSceneDocument;
    };

    struct EditorDocumentTab
    {
        DocumentId mDocument = kSceneDocument;
        std::string mLabel;
        bool mUnsaved = false;
        bool mClosable = false;
    };

    // The Scene first, then every open Prefab document.
    struct EditorDocuments
    {
        std::vector<EditorDocumentTab> mTabs;
        DocumentId mActive = kSceneDocument;
        bool mEnabled = true;
    };

    struct EditorActions
    {
        SceneMenuAction mScene = SceneMenuAction::None;
        std::string mOpenProject;
        std::optional<OpenSceneRequest> mOpenScene;

        // non-empty mCreateProjectName requests CreateGameProject(name, RT),
        // then the same open-into-new-directory path as mOpenProject
        std::string mCreateProjectName;
        bool mCreateProjectRayTracing = false;
        AssetImportAction mAssetImport;
        ContentBrowserAction mContentBrowser;
        EditorDocumentAction mDocument;
        bool mSaveActiveDocument = false;
        bool mToggleMaterialEditor = false;
        BuildAction mBuild;
        // Project Settings GI overlay and Fit-to-bounds input for the renderer.
        bool mShowGiVolume = false;
        bool mRequestGiSceneBounds = false;
    };

    class Editor
    {
    public:
        Editor() = default;
        ~Editor();

        Editor(const Editor &) = delete;
        Editor &operator=(const Editor &) = delete;
        Editor(Editor &&) = delete;
        Editor &operator=(Editor &&) = delete;

        // imgui must already be initialized; the editor never owns the context
        bool Initialize(const ImGuiRuntime &imgui, Window &window, std::filesystem::path projectSettingsPath,
            World &world, ISceneOps &sceneOps, bool srgbOutput);

//before ImGuiRuntime::Shutdown
        void Shutdown();

        //between ImGuiRuntime::BeginFrame and EndFrame
        EditorActions DrawLayout(bool enableLayout, InputMap &inputMap, RenderSettings &renderSettings,
            PostProcessingSettings &postProcessingSettings, const EngineModeState &mode,
            std::span<const ProjectMenuEntry> projects, std::string_view currentProject,
            World &activeWorld, World &sceneWorld, const EditorDocuments &documents,
            const AssetImportView &assetImport, const VulkanRenderer &renderer,
            const ContentBrowserView &contentBrowser, bool materialPanelVisible,
            std::string_view draftsWarning, const BuildJobView &build, float dt);

        VkRect2D SceneViewportRect() const { return mSceneViewportRect; }

        // The registration lives until it dies; extensions run in registration order.
        Registration AddExtension(IEditorExtension &extension) { return mExtensions.Add(extension); }
        size_t ContentThumbnailCapacity() const { return mPreferences.contentThumbnailCapacity; }

        // relative to the game directory, as Content Browser paths are
        void SetSourceAssetsRoot(std::string assetsRoot) { mSourceAssetsRoot = std::move(assetsRoot); }
        // empty leaves extensions with no way to ask for an IDE
        void SetOpenAssetInIde(std::function<void(std::string_view)> open) { mOpenAssetInIde = std::move(open); }

        std::vector<std::string> ContentBrowserCreateItems() const { return mExtensions.ContentBrowserCreateItems(); }
        void CreateContentBrowserItem(std::string_view item, std::string_view browserFolder);
        // true once an extension took the game-relative file
        bool OpenContentBrowserFile(std::string_view browserRelativePath);
        std::vector<std::string> UnsavedDraftNames() const { return mExtensions.UnsavedDraftNames(); }

        const std::string &ContentBrowserSelection() const { return mContentBrowserPanel.SelectedPath(); }

        void UpdateCamera(float dt);

        EditorCamera &SceneCamera() { return mCamera; }

        bool &ParticleEditorVisible() { return mShowParticles; }
        std::string TakeParticleOpenRequest() { return std::exchange(mParticleOpenRequest, {}); }
        // One undoable scene edit of a ParticleSystem's per-instance emitter offset.
        void SubmitParticleEmitterOffset(World &world, Entity entity, uint32_t placementId, const glm::vec3 &offset);
        int TakeParticleHistoryRequest() { return std::exchange(mParticleHistoryRequest, 0); }
        void SetParticleHistoryAvailability(bool undo, bool redo)
        {
            mParticleCanUndo = undo;
            mParticleCanRedo = redo;
        }

        // chosen in Edit only; Play keeps rendering the last choice
        const DebugViewSettings &SceneDebugView() const { return mDebugView; }

        bool IsInitialized() const { return mInitialized; }

        bool WantsCaptureKeyboard() const;
        bool WantsCaptureMouse() const;
        // the gizmo, the view pie or an extension such as a brush has the pointer in the scene viewport
        bool ViewportToolsHavePointer() const;
        // after the game UI's frame: a scene click is applied unless a game widget took it
        void ResolveViewportPick(World &activeWorld, bool gameUIOverWidget);

        void ClearHistory() { mHistory.Clear(); }

        // Edits made during Play run on the runtime World through a history of their own, so they never reach the
        // scene being authored, its unsaved mark or its undo steps. Stop discards them.
        void BeginPlayEdits(ISceneOps &ops)
        {
            mPlaySceneOps = &ops;
            mPlayHistory.Clear();
        }
        void EndPlayEdits()
        {
            mPlaySceneOps = nullptr;
            mPlayHistory.Clear();
        }
        void ClearPlayHistory() { mPlayHistory.Clear(); }

        // Scene, Play and document transitions cancel a pending snap.
        void CancelSnapToTerrain() { mSnapToTerrain.Cancel("The scene changed."); }

        // true while the scene has changes since its last save, shown as the title bar's *
        bool IsSceneUnsaved() const { return mHistory.IsUnsaved(); }

        // false: the scene counts as saved from now on, so the * disapears
        // true: the * stays until the next save
        void SetSceneUnsaved(bool unsaved) { mHistory.SetUnsaved(unsaved); }


        bool IsDocumentUnsaved(DocumentId document) const { return mHistory.IsUnsaved(document); }
        void SetDocumentUnsaved(DocumentId document, bool unsaved) { mHistory.SetUnsaved(document, unsaved); }


        void SetAuthoringBackend(IAuthoringBackend *backend) { mHistory.SetAuthoringBackend(backend); }
        void SetAuthoredEntities(IAuthoredEntities *authored) { mAuthoredEntities = authored; }

        // null = no Audio settings and no soundtrack picker
        void SetAudio(IEditorAudio *audio)
        {
            mAudio = audio;
            mInspectorPanel.SetAudio(audio);
        }

        // the game File > Build Settings is for
        void SetSceneCatalog(std::vector<ProjectScene> scenes, std::string startupId)
        {
            mSceneCatalog = std::move(scenes);
            mOpenSceneId = std::move(startupId);
        }
        void SetSceneCreationError(std::string message) { mSceneCreationError = std::move(message); }
        void SetOpenSceneId(std::string id) { mOpenSceneId = std::move(id); }

        void SetBuildGame(BuildGame game) { mBuildGame = std::move(game); }

    private:
        void DrawTitleBar();
        void DrawOpenProjectMenu(std::span<const ProjectMenuEntry> projects, std::string_view currentProject);
        void DrawOpenProjectConfirm(EditorActions &actions, std::string_view draftsWarning);
        void DrawViewportGizmo(const EngineModeState &mode, World &world, EditorActions &actions,
            DocumentId document);
        EditorExtensionContext MakeExtensionContext(const EngineModeState &mode, World &world, DocumentId document,
            ImDrawList *overlay);
        void BeginGizmoDrag(World &world, std::span<const Entity> selected);
        // Applies the pivot's change since the drag began to every target, as one merging command.
        void ApplyGizmoDrag(World &world);
        void DrawViewportPrefabDrop(bool documentTab, World &world);
        void UpdateViewportPicking(const EngineModeState &mode, World &world);
        // F moves the scene camera next to the primary selection.
        void FocusSelection(const EngineModeState &mode, World &world);
        void DrawSelectionHighlights(const EngineModeState &mode, World &world);
        void DrawComponentGizmos(const EngineModeState &mode, World &world);
        // Held Z over the scene viewport opens the view pie; Edit only.
        void UpdateViewPie(const EngineModeState &mode);
        void DrawViewPie(std::optional<ViewPieItem> hovered);
        void DrawDebugViewMenu(const EngineModeState &mode);
        void DrawNewProjectModal(EditorActions &actions, std::span<const ProjectMenuEntry> projects,
            std::string_view draftsWarning);
        void DrawEditMenu();
        // Retries a pending snap outside any panel's Draw; it only reads the World until an edit is ready.
        void PollSnapToTerrain(World &world, DocumentId document, const EngineModeState &mode, float dt);
        void StartSnapToTerrain(World &world, DocumentId document);
        void FinishSnapToTerrain(World &world, DocumentId document, const GroundQuery *ground, bool editing,
            bool transitionRequested);
        void UpdateHistoryShortcuts(EditorActions &actions, const EngineModeState &mode);
        // File > Build and Build and Run, and Ctrl+B, a build that cannot start says why in the toast
        void RequestBuild(EditorActions &actions, const BuildJobView &build, bool run);
        // Ctrl+B builds and runs, Ctrl+Shift+B opens Build Settings
        void UpdateBuildShortcuts(EditorActions &actions, const EngineModeState &mode, const BuildJobView &build);
        void ShowBuildNote(std::string note);
        // At the bottom right: the running build while Build Settings is hidden, then how it ended
        void DrawBuildToast(const BuildJobView &build, EditorActions &actions);

        // Each document keeps its own camera; a document opened first focuses on its root.
        void SwitchDocumentCamera(const EditorDocuments &documents, World &world);
        void FocusDocumentRoot(World &world);

        SceneMenuAction DrawPlayControls(const EngineModeState &mode);

        void ApplyPreferencesRequest(const PreferencesRequest &request);
        void ApplyTheme();
        // copies @p source into EditorPreferencesDir() and shows it; false keeps the current image
        bool SetBackgroundImage(const std::filesystem::path &source);
        void ClearBackgroundImage();

        bool mInitialized = false;
        bool mShowStyleEditor = false;
        bool mShowProjectSettings = false;
        bool mShowInputSettings = false;
        bool mShowPostProcessing = false;
        bool mShowAssetImport = false;
        bool mShowParticles = false;
        std::string mParticleOpenRequest;
        int mParticleHistoryRequest = 0;
        bool mParticleHistoryFocused = false, mParticleCanUndo = false, mParticleCanRedo = false;
        bool mShowContentBrowser = false;
        bool mShowPreferences = false;
        bool mShowProfiler = false;
        bool mShowBuildSettings = false;
        bool mFocusBuildSettings = false;
        // drawn this frame, not collapsed or behind another tab
        bool mBuildSettingsVisible = false;
        // the build the toast last noticed ending, when, and whether it was closed
        std::uint64_t mToastFinished = 0;
        double mToastAt = -1.0;
        bool mToastClosed = false;
        std::string mBuildNote;
        double mBuildNoteAt = -1.0;
        std::string mImGuiIniPath;
        std::filesystem::path mProjectSettingsPath;
        VkRect2D mSceneViewportRect{};
        Window *mWindow = nullptr;
        std::vector<PixelRect> mTitleBarInteractiveRects;
        LogPanel mLogPanel;
        HierarchyPanel mHierarchyPanel;
        AssetImportPanel mAssetImportPanel;
        ContentBrowserPanel mContentBrowserPanel;
        InspectorPanel mInspectorPanel;
        ProjectSettingsPanel mProjectSettingsPanel;
        PreferencesPanel mPreferencesPanel;
        BuildSettingsPanel mBuildSettingsPanel;
        BuildGame mBuildGame;
        InputSettingsPanel mInputSettingsPanel;
        PostProcessingPanel mPostProcessingPanel;
        ProfilerPanel mProfilerPanel;
        EditorExtensions mExtensions;
        std::string mSourceAssetsRoot;
        std::uint32_t mInspectorDockId = 0;
        std::function<void(std::string_view)> mOpenAssetInIde;
        EditorCamera mCamera;
        std::map<DocumentId, EditorCamera> mDocumentCameras;
        DocumentId mCameraDocument = kSceneDocument;

        // the tab strip reveals mActive only when it changed elsewhere; selecting it every frame would undo a click
        DocumentId mShownDocument = kSceneDocument;
        bool mShownDocumentKnown = false;

        History mHistory;
        History mPlayHistory;
        ISceneOps *mPlaySceneOps = nullptr;
        SnapToTerrainSession mSnapToTerrain;
        std::size_t mSnapFrameSubmissions = 0;
        ISceneOps *mSceneOps = nullptr;
        IAuthoredEntities *mAuthoredEntities = nullptr;

        bool PlayEditing() const { return mPlaySceneOps != nullptr; }
        History &ActiveHistory() { return PlayEditing() ? mPlayHistory : mHistory; }
        // Play has no authored entities, its edits are not written back to a scene.
        IAuthoredEntities *ActiveAuthored() const { return PlayEditing() ? nullptr : mAuthoredEntities; }
        IEditorAudio *mAudio = nullptr;

        std::filesystem::path mPreferencesPath;
        EditorPreferences mPreferences;
        BackgroundImage mBackground;
        bool mSrgbOutput = true;
        // applied at the next DrawLayout, before any style is pushed
        bool mThemePending = false;

        // waiting on the confirm dialog
        std::vector<ProjectScene> mSceneCatalog;
        std::string mOpenSceneId;
        std::string mPendingSceneId;
        std::string mPendingScenePath;
        std::string mSceneCreationError;
        char mNewSceneId[128]{};
        char mNewScenePath[512]{};
        ProjectMenuEntry mPendingProject;
        bool mOpenConfirmRequested = false;

        GizmoOperation mGizmoOperation = GizmoOperation::Translate;
        GizmoSpace mGizmoSpace = GizmoSpace::Local;

        bool mMouseWasDown = false;

        // last frame's ImGuizmo::IsUsing
        bool mGizmoWasUsing = false;

        DebugViewSettings mDebugView;

        bool mViewPieOpen = false;
        bool mGizmoHasPointer = false;

        struct PendingPick
        {
            World *mWorld = nullptr;
            Entity mEntity;
            bool mAdditive = false;
        };
        std::optional<PendingPick> mPendingPick;
        // Z came up inside the dead zone: the pie stays until a click or Escape
        bool mViewPieSticky = false;
        glm::vec2 mViewPieCenter{ 0.0f };

        struct GizmoTarget
        {
            Entity mEntity;
            WorldPose mParent;
            WorldOffset mFromPivot;
            Transform mStart;
        };

        // kept between frames while dragged, so the drag is measured from where it began
        WorldPose mGizmoPivot;
        WorldPose mGizmoDragStart;
        std::vector<GizmoTarget> mGizmoTargets;

        // New Project modal
        bool mNewProjectRequested = false;
        char mNewProjectName[64] = {};
        bool mNewProjectRayTracing = false;
    };
} // namespace mir
