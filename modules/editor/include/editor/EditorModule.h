/**
 * @file EditorModule.h
 * @author sumin.park
 * @brief The editor: its UI drawer, scene ops, asset import and content browser.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/AssetImportManager.h>
#include <editor/ContentBrowserWatcher.h>
#include <editor/Editor.h>
#include <editor/PrefabAuthoringAdapter.h>
#include <editor/PrefabPlayMarks.h>
#include <editor/PrefabWorkspace.h>
#include <editor/WorldSceneOps.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>
#include <project/GameProject.h>

#include <optional>
#include <string>
#include <vector>

namespace mir
{
    class AssetsModule;
    class ImGuiModule;
    class PrefabModule;
    class RendererModule;
    class SceneModule;
    class WindowModule;

    class EditorModule
    {
    public:
        // enableLayout false: the editor is built in but the window is the game (--no-editor)
        EditorModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window, RendererModule &renderer,
            SceneModule &scene, PrefabModule &prefab, AssetsModule &assets, const GameProject &project,
            bool enableLayout);
        ~EditorModule();

        EditorModule(const EditorModule &) = delete;
        EditorModule &operator=(const EditorModule &) = delete;

        Editor &Get() { return mEditor; }
        const Editor &Get() const { return mEditor; }
        AssetImportManager &Import() { return mImport; }
        bool LayoutEnabled() const { return mEnableLayout; }

    private:
        // what the menus, toolbar and panels asked for this frame
        void ApplyActions(const EditorActions &actions);
        void OpenInExternalIde(const std::string &relativePath);
        void OpenPrefabAsset(const std::string &relativePath);

        // Save Prefab on a Prefab tab; on the Scene tab, the unsaved prefabs it uses, then the scene
        void SaveActiveDocument();

        // the active Prefab tab's preview World in Edit, else null
        World *ActivePreviewWorld();
        EditorDocuments MakeEditorDocuments();
        PrefabWorkspace::PrefabUnsaved UnsavedPrefabs() const;
        PrefabWorkspace::PrefabSaved MarkSavedPrefabs();

        // after DrawLayout: the scene viewport's rect, and in Edit the editor camera
        void FeedViewport();

        World &mWorld;
        RendererModule &mRenderer;
        SceneModule &mScene;
        PrefabModule &mPrefab;
        GameProject mProject;
        bool mEnableLayout;
        float mFrameDt = 0.0f;
        std::vector<ProjectMenuEntry> mProjectMenu;

        AssetImportManager mImport;
        ContentBrowserWatcher mContentBrowser;
        WorldSceneOps mSceneOps;
        // before mEditor: they die after it, once Shutdown stopped it using them
        PrefabAuthoringAdapter mPrefabAuthoring;
        PrefabWorkspace mPrefabWorkspace;
        Editor mEditor;

        // Stop puts the World back as it was at Play, so it puts this back too
        bool mUnsavedBeforePlay = false;
        // Play shows the Scene tab; Stop reopens the Prefab tab it left
        std::optional<PrefabId> mPrefabBeforePlay;
        // the Prefab tabs' marks: Stop restores them with the World
        PrefabPlayMarks mPrefabMarksBeforePlay;

        // a Prefab file changed under an open document: its save point no longer holds
        void RefreshPrefabHistory();

        // last: they unregister before anything above is destroyed
        Registration mFrameBegin;
        Registration mDrawer;
        Registration mSceneUnload;
        Registration mSceneLoad;
        Registration mSceneSaved;
        Registration mPlayBegin;
        Registration mReloadPrepare;
        Registration mReloadCommit;
        Registration mReloadAbort;
    };
}
