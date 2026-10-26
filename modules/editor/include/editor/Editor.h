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
#include <core/platform/Surface.h>
#include <editor/BackgroundImage.h>
#include <editor/EditorCamera.h>
#include <editor/EditorPreferences.h>
#include <editor/SnapToTerrain.h>
#include <editor/history/History.h>
#include <editor/panels/AssetImportPanel.h>
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
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/VulkanRenderer.h>
#include <window/Window.h>

#include <filesystem>
#include <map>
#include <span>
#include <string>
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

        // non-empty mCreateProjectName requests CreateGameProject(name, RT),
        // then the same open-into-new-directory path as mOpenProject
        std::string mCreateProjectName;
        bool mCreateProjectRayTracing = false;
        AssetImportAction mAssetImport;
        ContentBrowserAction mContentBrowser;
        EditorDocumentAction mDocument;
        bool mSaveActiveDocument = false;
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
                                 const ContentBrowserView &contentBrowser, float dt);

        VkRect2D SceneViewportRect() const { return mSceneViewportRect; }

        void UpdateCamera(float dt);

        EditorCamera &SceneCamera() { return mCamera; }

        bool IsInitialized() const { return mInitialized; }

        bool WantsCaptureKeyboard() const;
        bool WantsCaptureMouse() const;

        void ClearHistory() { mHistory.Clear(); }

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

    private:
        void DrawTitleBar();
        void DrawOpenProjectMenu(std::span<const ProjectMenuEntry> projects, std::string_view currentProject);
        void DrawOpenProjectConfirm(EditorActions &actions);
        void DrawViewportGizmo(const EngineModeState &mode, World &world, EditorActions &actions,
            bool prefabDocument);
        void DrawViewportPrefabDrop(bool documentTab, World &world);
        void UpdateViewportPicking(const EngineModeState &mode, World &world);
        // F moves the scene camera next to the primary selection.
        void FocusSelection(const EngineModeState &mode, World &world);
        void DrawSelectionHighlights(const EngineModeState &mode, World &world);
        void DrawComponentGizmos(const EngineModeState &mode, World &world);
        void DrawNewProjectModal(EditorActions &actions, std::span<const ProjectMenuEntry> projects);
        void DrawEditMenu();
        // Retries a pending snap outside any panel's Draw; it only reads the World until an edit is ready.
        void PollSnapToTerrain(World &world, DocumentId document, const EngineModeState &mode, float dt);
        void StartSnapToTerrain(World &world, DocumentId document);
        void FinishSnapToTerrain();
        void UpdateHistoryShortcuts(EditorActions &actions, const EngineModeState &mode);

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
        bool mShowContentBrowser = false;
        bool mShowPreferences = false;
        bool mShowProfiler = false;
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
        InputSettingsPanel mInputSettingsPanel;
        PostProcessingPanel mPostProcessingPanel;
        ProfilerPanel mProfilerPanel;
        EditorCamera mCamera;
        std::map<DocumentId, EditorCamera> mDocumentCameras;
        DocumentId mCameraDocument = kSceneDocument;

        // the tab strip reveals mActive only when it changed elsewhere; selecting it every frame would undo a click
        DocumentId mShownDocument = kSceneDocument;
        bool mShownDocumentKnown = false;

        History mHistory;
        SnapToTerrainSession mSnapToTerrain;
        ISceneOps *mSceneOps = nullptr;
        IAuthoredEntities *mAuthoredEntities = nullptr;

        std::filesystem::path mPreferencesPath;
        EditorPreferences mPreferences;
        BackgroundImage mBackground;
        bool mSrgbOutput = true;
        // applied at the next DrawLayout, before any style is pushed
        bool mThemePending = false;

        // waiting on the confirm dialog
        ProjectMenuEntry mPendingProject;
        bool mOpenConfirmRequested = false;

        GizmoOperation mGizmoOperation = GizmoOperation::Translate;
        GizmoSpace mGizmoSpace = GizmoSpace::Local;

        bool mMouseWasDown = false;

        // last frame's ImGuizmo::IsUsing
        bool mGizmoWasUsing = false;

        // New Project modal
        bool mNewProjectRequested = false;
        char mNewProjectName[64] = {};
        bool mNewProjectRayTracing = false;
    };
} // namespace mir
