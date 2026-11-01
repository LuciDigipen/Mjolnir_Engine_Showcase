/**
 * @file App.cpp
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>

#include "Prompt.h"
#include <assets/AssetIdRoot.h>
#include <assets/AssetSettings.h>
#include <core/EngineVersion.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <core/platform/CrashHandler.h>
#include <input/DefaultInputMap.h>
#include <input/InputState.h>
#include <prefab/PrefabCoordinator.h>
#include <project/ProjectSettings.h>
#include <scene/SceneIO.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        size_t MissingFileCount(const SceneProbe &probe)
        {
            return probe.mMissingEntities.size() + probe.mMissingInstances.size();
        }

        // relative to the scene directory, e.g. "entities/2.json, instances/5.json"
        std::string MissingFileList(const SceneProbe &probe)
        {
            std::string files;
            const auto append = [&files](const char *dir, const std::vector<StableId> &ids)
                {
                    for (const StableId id : ids)
                        files += (files.empty() ? "" : ", ") + std::string(dir) + "/" + std::to_string(id) + ".json";
                };
            append("entities", probe.mMissingEntities);
            append("instances", probe.mMissingInstances);
            return files;
        }

        std::unique_ptr<AsyncAssetService> MakeAssetService(
            AssetsModule &assets, JobExecutor &jobs, const std::filesystem::path &projectSettingsPath)
        {
            if (!assets.HadManifest())
                return nullptr;
            return std::make_unique<AsyncAssetService>(
                assets, jobs, AssetSettings::LoadFile(projectSettingsPath).loadThreads);
        }

        std::filesystem::path PrefabSourceDir(const GameProject &project)
        {
            const std::string prefix = project.mDir + "/";
            if (project.mAssetIdRoot.rfind(prefix, 0) == 0)
            {
                return project.mRootDir / project.mAssetIdRoot.substr(prefix.size()) /
                    std::string(kPrefabDirectoryName);
            }
            return project.mRootDir / "assets" / std::string(kPrefabDirectoryName);
        }
    }

    namespace
    {
        // the checks that come before anything is built
        AppDesc Prepared(const AppDesc &desc)
        {
            AppDesc prepared = desc;
            EnsureGameDirectories(prepared.mProject);
            ReadProjectSettings(prepared.mProject.mProjectSettingsPath);

        #if !MIR_WITH_EDITOR
            //no editor to lay out, the window is the game
            prepared.mEnableEditorLayout = false;
            MIR_LOG_INFO("Built without the editor; running the game");
        #endif

            EnsureDpiAware();
            return prepared;
        }

        // the title points into desc, which outlives the window's creation
        WindowDesc MakeWindowDesc(const AppDesc &desc)
        {
            WindowDesc window{};
            window.mWidth = desc.mWidth;
            window.mHeight = desc.mHeight;
            window.mTitle = desc.mProject.mTitle.c_str();
            window.mMaximized = desc.mEnableEditorLayout;
            window.mCustomTitleBar = desc.mEnableEditorLayout;
            window.mStartHidden = true;
            return window;
        }
    }

    App::StartupSplash::StartupSplash(const AppDesc &desc)
    {
        if (!desc.mHidden) screen.Show({ .mEngineName = desc.mAppName,
                      .mVersion = kEngineVersion,
                      .mCopyright = kEngineCopyright,
                      .mStatus = "Creating window...",
                      .mProgress = 0.0f });
    }

    App::App(const AppDesc &desc)
        : mDesc(Prepared(desc)),
          mProject(mDesc.mProject),
          mSplash(mDesc),
          mJobs(mDesc.mWorkerThreads),
          mCore(Context(), mCommands),
          mAssets(CookedAssetsDir()),
          mAssetService(MakeAssetService(mAssets, mJobs, mProject.mProjectSettingsPath)),
          mResolver(mAssets, mProject.mAssetIdRoot),
          mParticles(Context(), mAssets, mResolver),
          mScene(Context(), mProject.mSceneDir),
          mPrefab(Context(), mScene, PrefabSourceDir(mProject), kHasEditor ? nullptr : &mAssets),
          mGameUI(Context()),
          mGameShell(Context(), mGameUI, mAssets, mResolver,
              GameShellSettings{ .mUIDemo = mDesc.mUIDemo, .mQuitStopsPlay = mDesc.mEnableEditorLayout }),
          mWindow(MakeWindowDesc(mDesc)),
          mRenderer(Context(), mWindow.Get(), mAssets,
              RendererModuleSettings{ .mAppName = mDesc.mAppName,
                                      .mEnableValidation = mDesc.mEnableValidation,
                                      .mProjectSettingsPath = mProject.mProjectSettingsPath,
                                      .mResolver = &mResolver }),
          mParticlesGpu(mRenderer.Renderer()),
          mInput(Context(), mWindow.Get(), mProject.mProjectSettingsPath),
          mAnimation(Context(), mRenderer.Renderer(), mAssets, mResolver, &mJobs),
          mImGui(Context(), mWindow, mRenderer),
    #if MIR_WITH_EDITOR
        mEditor(Context(), mImGui, mWindow, mRenderer, mScene, mPrefab, mAssets, mResolver, mProject,
            mDesc.mEnableEditorLayout),
    #endif
          mCamera(Context()),
    #if MIR_WITH_AUDIO
          mAudio(Context(), mProject.mSoundBankDir, mProject.mProjectSettingsPath,
              UserDataDir() / mProject.mName / "settings.json"),
    #endif
          mPhysics(Context(), mProject.mProjectSettingsPath, &mJobs),
          mTerrainData(Context()),
          mScript(Context(), mAssets, mResolver),
          mParticlesRuntime(Context(), mParticles, mParticlesGpu, mRenderer.Rendering()),
          mParticlesScript(mScript),
    #if MIR_WITH_AUDIO
          mAudioScript(mScript),
    #endif
          mInputScript(mScript),
          mPhysicsScript(mScript),
          mRendererScript(mScript),
          mTerrainScript(mScript),
          mTerrain(Context(), mRenderer, mProject, mResolver, mJobs),
          mGameUIImGui(Context(), mImGui, mWindow, mGameUI),
          mFixedTimestep{ .mStep = mDesc.mFixedDeltaSeconds, .mMaxSteps = mDesc.mMaxFixedSteps }
    {
    #if MIR_WITH_AUDIO
        mGameShell.SetAudioOptions(&mAudioOptions);
    #if MIR_WITH_EDITOR
        mEditor.SetAudio(&mEditorAudio);
    #endif
    #endif

        mSplash.screen.SetProgress("Loading scene...", 0.9f);
        LoadProjectScene();

        //without the editor there is no Play button
        if (!mDesc.mEnableEditorLayout)
            PlayScene();

        // every module is built, so a library that took the crash filter slot has done so
        ReclaimCrashHandler();

        if (!mDesc.mHidden) mWindow.Get().Show();
    }

    // Systems stop while every module is alive; then members die in reverse, the
    // modules before the scheduler, so a module's system pointers stay valid to the end
    App::~App()
    {
        SystemContext stopContext = MakeContext(0.0f);
        mScheduler.Stop(stopContext);
        MIR_LOG_INFO("App shut down");
    }

    bool App::RequestOpenProject(const std::string &requestedDir)
    {
        // validate before closing; not ResolveGameProject, which logs "Loading game"
        try
        {
            // set only once it can open: main relaunches into whatever is here
            GameProject next = LoadGameProject(NormalizeGameDir(requestedDir, ErrorCategory::Usage));
            ResolveAssetIdRoot(next);
            mProjectToOpen = std::move(next);
        }
        catch (const SetupError &error)
        {
            MIR_LOG_ERROR("App::RequestOpenProject: '{}' cannot be opened ({}: {}), '{}' stays open", requestedDir,
                error.What(), error.Cause(), mProject.mDir);
            return false;
        }

        // one game per process: main.cpp relaunches after Run returns
        MIR_LOG_INFO("Closing '{}' to open '{}'", mProject.mDir, mProjectToOpen->mDir);
        mWindow.Get().RequestClose();
        return true;
    }

    void App::NewScene(std::string name, bool incomplete)
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::NewScene: ignored while Play is running (Stop first)");
            return;
        }

        UnloadCurrentScene();
        mScene.New(std::move(name), incomplete);
        RunStage(Stage::SceneLoad);
    }

    bool App::SaveScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::SaveScene: ignored while Play is running (Stop first)");
            return false;
        }

        if (!mScene.IsBound())
        {
            if (!ConfirmBindUntitledScene())
            {
                MIR_LOG_WARN("App::SaveScene: the untitled scene was not saved, '{}' is left as it is",
                    mProject.mSceneDir.string());
                return false;
            }
            mScene.Bind();
        }

        const bool saved = mScene.Save(&mPrefab.Coordinator().Resolver());
        if (saved)
            RunStage(Stage::SceneSaved);
        return saved;
    }

    bool App::LoadScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::LoadScene: ignored while Play is running (Stop first)");
            return false;
        }

        if (!std::filesystem::exists(SceneManifestPath(mProject.mSceneDir)))
        {
            MIR_LOG_ERROR("App::LoadScene: no {} in '{}'", kSceneManifestFileName, mProject.mSceneDir.string());
            return false;
        }

        // probed before unloading, so refusing keeps the current scene intact
        const SceneProbe probe = ProbeScene(mProject.mSceneDir);
        switch (ChooseSceneLoad(probe, false))
        {
        case SceneLoadChoice::Refuse:
            MIR_LOG_WARN("App::LoadScene: kept the current scene over the broken one in '{}'",
                mProject.mSceneDir.string());
            return false;
        case SceneLoadChoice::StartEmpty:
            NewScene("untitled", true);
            return true;
        case SceneLoadChoice::Load:
            break;
        }

        ReplaceSceneFromDisk(!probe.Ok());
        return true;
    }

    void App::ReplaceSceneFromDisk(bool incomplete)
    {
        UnloadCurrentScene();
        mScene.LoadFromDisk(&mPrefab.Coordinator().Resolver(), incomplete);
        RunStage(Stage::SceneLoad);
    }

    App::SceneLoadChoice App::ChooseSceneLoad(const SceneProbe &probe, bool atStartup) const
    {
        if (probe.Ok())
            return SceneLoadChoice::Load;

        // a game build has nobody who could repair the scene, so it never offers to
        if (!kHasEditor)
            return SceneLoadChoice::Refuse;

        const std::string refuse = atStartup ? "Choose No to quit." : "Choose No to keep the current scene.";
        const std::string manifest = SceneManifestPath(mProject.mSceneDir).string();

        if (!probe.mManifestError.empty())
        {
            const std::string message = manifest + " cannot be read (" + probe.mManifestError +
                ").\n\nStart with an empty, untitled scene? The broken scene stays on disk as it is, and Save "
                "asks before replacing it.\n\n" + refuse;
            return AskYesNo("Scene file is corrupt", message, mDesc.mNonInteractive) ? SceneLoadChoice::StartEmpty
                : SceneLoadChoice::Refuse;
        }

        const std::string message = std::to_string(MissingFileCount(probe)) + " file(s) listed in " + manifest +
            " are missing or unreadable (" + MissingFileList(probe) + ").\n\nOpen the scene without them? Saving it will drop them for good; restore them from git first "
            "if you need them.\n\n" + refuse;
        return AskYesNo("Scene has missing entities", message, mDesc.mNonInteractive) ? SceneLoadChoice::Load
            : SceneLoadChoice::Refuse;
    }

    bool App::ConfirmBindUntitledScene() const
    {
        const std::string sceneDir = mProject.mSceneDir.string();
        const std::string message = "This scene is untitled. It was started empty in place of the scene in " +
            sceneDir + ", which could not be read.\n\nSave it there? That replaces " +
            std::string(kSceneManifestFileName) + " and every entity and instance file in " + sceneDir +
            "; restore them from git first if you need them. To open the old scene instead, restore it and "
            "choose File > Load Scene.\n\nChoose No to leave the files as they are.";
        return AskYesNo("Save the untitled scene", message, mDesc.mNonInteractive);
    }

    void App::PlayScene()
    {
        if (IsPlaying(mWorld))
            return;

        // before the snapshot: the prefabs commit and freeze, so it holds what Play runs
        RunStage(Stage::PlayBegin);
        mScene.CaptureForPlay(&mPrefab.Coordinator().Resolver());

        SetPlayState(mWorld, PlayState::Running);
        SetEngineMode(mWorld, EngineMode::Play);
    }

    void App::StopScene()
    {
        if (!IsPlaying(mWorld))
            return;

        // Play's entities fire their OnRemove as the restore destroys them; the GPU caches,
        // keyed by asset, stay for the restored scene
        RunStage(Stage::PlayEnd);
        mScene.RestoreFromPlay(&mPrefab.Coordinator().Resolver());

        SetEngineMode(mWorld, EngineMode::Edit);
        SetPlayState(mWorld, PlayState::Running);

        // terrain starts checking a release published during Play
        RunStage(Stage::SceneLoad);
    }

    void App::TogglePause()
    {
        if (!IsPlaying(mWorld))
            return;

        SetPlayState(mWorld, IsPaused(mWorld) ? PlayState::Running : PlayState::Paused);
    }

    void App::ApplyRequests()
    {
        RequestQueue &queue = mWorld.Resource<RequestQueue>();
        if (const std::optional<std::string> dir = queue.TakeOpenProject(); dir.has_value())
            RequestOpenProject(*dir);

        for (const Request request : queue.Take())
        {
            switch (request)
            {
            case Request::Play:
                PlayScene();
                break;
            case Request::Stop:
                StopScene();
                break;
            case Request::Pause:
                if (IsSimulating(mWorld))
                    TogglePause();
                break;
            case Request::Resume:
                if (IsPaused(mWorld))
                    TogglePause();
                break;
            case Request::NewScene:
                NewScene();
                break;
            case Request::LoadScene:
                LoadScene();
                break;
            case Request::SaveScene:
                SaveScene();
                break;
            case Request::ApplyImportedAssets:
            #if MIR_WITH_EDITOR
                // the import may have been applied or dropped since it asked
                if (mEditor.Import().PendingApply() && IsEditing(mWorld))
                    ApplyImportedAssets();
            #endif
                break;
            case Request::Quit:
                mWindow.Get().RequestClose();
                break;
            }
        }
    }

    void App::ToggleFullscreen()
    {
        mWindow.Get().ToggleFullscreen();

        mRenderer.Renderer().RequestSwapchainRecreate();
    }

    bool App::HasUnsavedMaterialDrafts() const
    {
#if MIR_WITH_EDITOR
        return mEditor.HasUnsavedMaterialDrafts();
#else
        return false;
#endif
    }

    bool App::ConfirmEditorExit()
    {
        if (!HasUnsavedMaterialDrafts())
            return true;
        if (mDesc.mNonInteractive || mDesc.mMaxFrames != 0)
        {
            MIR_LOG_WARN("Closing with unsaved material drafts in a bounded run; they are not saved automatically.");
            return true;
        }
        return AskYesNo("Unsaved material drafts",
            "Close and discard the unsaved material drafts? They are never saved automatically.", false);
    }

    void App::UnloadCurrentScene()
    {
        mScene.Unload();
        RunStage(Stage::SceneUnload);
    }

    void App::RunStage(Stage stage)
    {
        SystemContext context = MakeContext(0.0f);
        mScheduler.RunStage(stage, context);
    }

    void App::LoadProjectScene()
    {
        const std::filesystem::path manifest = SceneManifestPath(mProject.mSceneDir);

        std::error_code ec;
        if (!std::filesystem::exists(manifest, ec))
        {
            MIR_LOG_INFO("No {} in '{}', starting with an empty scene", kSceneManifestFileName, mProject.mSceneDir.string());
            mScene.New("untitled");
            RunStage(Stage::SceneLoad);
            return;
        }

        const SceneProbe probe = ProbeScene(mProject.mSceneDir);
        switch (ChooseSceneLoad(probe, true))
        {
        case SceneLoadChoice::StartEmpty:
            MIR_LOG_WARN("Starting with an untitled empty scene, the corrupt '{}' is left as it is", manifest.string());
            mScene.New("untitled", true);
            RunStage(Stage::SceneLoad);
            return;
        case SceneLoadChoice::Load:
            ReplaceSceneFromDisk(!probe.Ok());
            return;
        case SceneLoadChoice::Refuse:
            break;
        }

        if (!probe.mManifestError.empty())
        {
            throw SetupError(ErrorCategory::ProjectData, "scene.json is corrupt", manifest, probe.mManifestError,
                "Restore " + manifest.string() + " from git.");
        }

        throw SetupError(ErrorCategory::ProjectData,
            std::to_string(MissingFileCount(probe)) + " file(s) of the scene are missing or unreadable",
            mProject.mSceneDir, "missing or unreadable: " + MissingFileList(probe),
            "Restore those files from git, or open the scene in the editor and choose to load it without them.");
    }

    void App::RunSwitchHook()
    {
        if (mSwitchHookFired || mDesc.mSwitchProjectTo.empty() || mDesc.mMaxFrames == 0)
            return;

        if (mFrame < mDesc.mMaxFrames / 2)
            return;

        // once, even if it fails
        mSwitchHookFired = true;
        RequestOpenProject(mDesc.mSwitchProjectTo);
    }

    ModuleContext App::Context()
    {
        return ModuleContext{ mWorld, mScheduler };
    }

    SystemContext App::MakeContext(float dt)
    {
        return SystemContext{ mWorld, mCommands, dt, mElapsed, mFrame, mFixedTimestep.Alpha() };
    }

    void App::Run()
    {
        SystemContext startContext = MakeContext(0.0f);
        mScheduler.Start(startContext);

        auto previous = std::chrono::steady_clock::now();

        while (true)
        {
            if (mWindow.Get().ShouldClose())
            {
                if (!mProjectToOpen.has_value() && !ConfirmEditorExit())
                    mWindow.Get().CancelClose();
                else
                    break;
            }

            mWindow.Get().PollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), mDesc.mMaxDeltaSeconds);
            previous = now;
            mElapsed += dt;

            // nothing is drawn or running yet, so a transition frees nothing in use
            ApplyRequests();
            if (mAssetService)
                mAssetService->DrainCompletions();

            SystemContext frameContext = MakeContext(dt);
            mScheduler.RunFrameSlot(FrameSlot::FrameBegin, frameContext);

            mImGui.RunUI();

            // advances in every mode; FixedUpdate systems check IsSimulating
            // themselves, as ScriptSystem does
            const uint32_t fixedSteps = mFixedTimestep.Advance(dt);

            SystemContext context = MakeContext(dt);
            if (!mDesc.mCapturePath.empty() && mFrame + 1 == mDesc.mMaxFrames)
                mRenderer.Renderer().CaptureNextFrame(mDesc.mCapturePath);
            mScheduler.Update(context, fixedSteps, mFixedTimestep.mStep);

            if (mWorld.Resource<InputState>().JustPressed(kToggleFullscreenAction))
                ToggleFullscreen();

            ++mFrame;

            RunSwitchHook();

            // same exit path as the close button
            if (mDesc.mMaxFrames != 0 && mFrame >= mDesc.mMaxFrames)
                mWindow.Get().RequestClose();
        }
    }

#if MIR_WITH_EDITOR
    void App::ApplyImportedAssets()
    {
        // a ReloadPrepare function vetoes by throwing; ReloadAbort drops what the others staged
        try
        {
            mAssets.BeginReload();
            RunStage(Stage::ReloadPrepare);
        }
        catch (const std::exception &error)
        {
            RunStage(Stage::ReloadAbort);
            mAssets.AbortReload();
            mEditor.Import().MarkApplyFailed(error.what());
            return;
        }

        mAssets.CommitReload();
        RunStage(Stage::ReloadCommit);
    }
#endif
} // namespace mir
