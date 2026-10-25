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
#include <editor/WorldSceneOps.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>
#include <project/GameProject.h>

#include <string>
#include <vector>

namespace mir
{
    class AssetsModule;
    class ImGuiModule;
    class RendererModule;
    class SceneModule;
    class WindowModule;

    class EditorModule
    {
    public:
        // enableLayout false: the editor is built in but the window is the game (--no-editor)
        EditorModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window, RendererModule &renderer,
            SceneModule &scene, AssetsModule &assets, const GameProject &project, bool enableLayout);
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

        // after DrawLayout: the scene viewport's rect, and in Edit the editor camera
        void FeedViewport();

        World &mWorld;
        RendererModule &mRenderer;
        GameProject mProject;
        bool mEnableLayout;
        float mFrameDt = 0.0f;
        std::vector<ProjectMenuEntry> mProjectMenu;

        AssetImportManager mImport;
        ContentBrowserWatcher mContentBrowser;
        WorldSceneOps mSceneOps;
        Editor mEditor;

        // Stop puts the World back as it was at Play, so it puts this back too
        bool mUnsavedBeforePlay = false;

        // last: they unregister before anything above is destroyed
        Registration mFrameBegin;
        Registration mDrawer;
        Registration mSceneLoad;
        Registration mSceneSaved;
        Registration mPlayBegin;
        Registration mReloadCommit;
    };
}
