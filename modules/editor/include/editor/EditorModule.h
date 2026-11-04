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
#include <editor/BuildJob.h>
#include <editor/ContentBrowserWatcher.h>
#include <editor/Editor.h>
#include <editor/EditorAudio.h>
#include <editor/PrefabAuthoringAdapter.h>
#include <editor/PrefabPlayMarks.h>
#include <editor/PrefabWorkspace.h>
#include <editor/ShaderHotReload.h>
#include <editor/WorldSceneOps.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>
#include <project/GameProject.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    class AssetResolver;
    class AssetsModule;
    class AsyncAssetService;
    class ParticleEditorPanel;
    class ImGuiModule;
    class JobExecutor;
    class MaterialEditor;
    class PrefabModule;
    class RendererModule;
    class SceneModule;
    class ModelLibrary;
    class WindowModule;

    class EditorModule
    {
    public:
        // enableLayout false: the editor is built in but the window is the game (--no-editor)
        EditorModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window, RendererModule &renderer,
            SceneModule &scene, PrefabModule &prefab, AssetsModule &assets, AsyncAssetService *assetService,
            const AssetResolver &resolver, const GameProject &project, JobExecutor &jobs, bool enableLayout,
            std::shared_ptr<ModelLibrary> models);
        ~EditorModule();

        EditorModule(const EditorModule &) = delete;
        EditorModule &operator=(const EditorModule &) = delete;

        Editor &Get() { return mEditor; }
        const Editor &Get() const { return mEditor; }
        AssetImportManager &Import() { return mImport; }
        bool LayoutEnabled() const { return mEnableLayout; }
        bool HasUnsavedMaterialDrafts() const;

        // set once the audio is built, null leaves out the Audio settings and the soundtrack picker
        void SetAudio(IEditorAudio *audio)
        {
            mAudio = audio;
            mEditor.SetAudio(audio);
        }

        // Build with unsaved changes takes Cancel without asking
        void SetNonInteractive(bool nonInteractive) { mNonInteractive = nonInteractive; }

    private:
        // what the menus, toolbar and panels asked for this frame
        void ApplyActions(const EditorActions &actions);
        void OpenInExternalIde(const std::string &relativePath);
        void OpenPrefabAsset(const std::string &relativePath);

        // Save Prefab on a Prefab tab
        void SaveActiveDocument();

        // the active Prefab tab's preview World in Edit, else null
        World *ActivePreviewWorld();
        EditorDocuments MakeEditorDocuments();

        // the browser's files, plus the new Prefabs that have no file yet
        ContentBrowserView MakeContentBrowserView();
        PrefabWorkspace::PrefabUnsaved UnsavedPrefabs() const;
        PrefabWorkspace::PrefabSaved MarkSavedPrefabs();

        // File > Build Settings' Build, which packages what is on disk
        void ApplyBuildAction(const BuildAction &action);
        void StartBuild(const BuildAction &action);
        
        std::string BuildBlockedReason() const;
        // empty when everything is saved
        std::string UnsavedForBuild() const;
        // every unsaved Prefab, then the scene, which is written at the top of the next frame
        bool SaveForBuild();
        BuildJobView MakeBuildJobView() const;

        
        void FeedViewport();

        World &mWorld;
        RendererModule &mRenderer;
        SceneModule &mScene;
        PrefabModule &mPrefab;
        const AssetResolver &mResolver;
        GameProject mProject;
        AssetsModule *mAssets = nullptr;
        bool mEnableLayout;
        float mFrameDt = 0.0f;
        std::vector<ProjectMenuEntry> mProjectMenu;

        AssetImportManager mImport;
        ContentBrowserWatcher mContentBrowser;
        ShaderHotReload mShaderHotReload;
        // before mSceneOps and mEditor: they die after them, once Shutdown stopped the editor using them
        PrefabAuthoringAdapter mPrefabAuthoring;
        PrefabWorkspace mPrefabWorkspace;
        WorldSceneOps mSceneOps;
        Editor mEditor;
        std::unique_ptr<ParticleEditorPanel> mParticleEditor;
        std::unique_ptr<MaterialEditor> mMaterialEditor;
        bool mShowMaterialEditor = false;

        // Stop puts the World back as it was at Play, so it puts this back too
        bool mUnsavedBeforePlay = false;
        // Play shows the Scene tab, Stop reopens the Prefab tab it left
        std::optional<PrefabId> mPrefabBeforePlay;
        // the Prefab tabs' marks, Stop restores them with the World
        PrefabPlayMarks mPrefabMarksBeforePlay;

        IEditorAudio *mAudio = nullptr;
        // Stop puts the volumes back too, so trying them in Play changes nothing for good
        std::optional<EditorAudioLevels> mAudioBeforePlay;

        BuildJob mBuild;
        std::filesystem::path mBuildTool;
        // the build waiting on the Save prompt or on the save
        BuildAction mPendingBuild;
        // what is unsaved, while the Save prompt is up
        std::string mBuildAskSave;
        bool mBuildAfterSave = false;
        bool mNonInteractive = false;

        
        void RefreshPrefabHistory();

        Registration mFrameBegin;
        Registration mDrawer;
        Registration mSceneUnload;
        Registration mSceneLoad;
        Registration mSceneSaved;
        Registration mPlayBegin;
        Registration mPlayEnd;
        Registration mReloadCommit;
    };
}
