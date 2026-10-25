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
#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>
#include <assets/AssetIdRoot.h>
#include <core/EngineVersion.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <core/platform/CrashHandler.h>
#include <input/DefaultInputMap.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <prefab/PrefabCoordinator.h>
#include <project/ProjectSettings.h>
#include <renderer/MeshResolveSystem.h>
#include <scene/SceneIO.h>
#include <script/ScriptModule.h>
#include <script/ScriptResolveSystem.h>
#include <script/ScriptSystem.h>
#include <terrain_runtime/TerrainRuntime.h>

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
    #if MIR_WITH_EDITOR
        UIRect ToUIRect(const VkRect2D &rect)
        {
            return UIRect{ static_cast<float>(rect.offset.x), static_cast<float>(rect.offset.y),
                          static_cast<float>(rect.extent.width), static_cast<float>(rect.extent.height) };
        }
    #endif

        UIPointer ToUIPointer(const RawInputSnapshot &input)
        {
            return UIPointer{ static_cast<float>(input.mouseX), static_cast<float>(input.mouseY),
                             input.IsDown(MouseButton::Left) };
        }

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
        screen.Show({ .mEngineName = desc.mAppName,
                      .mVersion = kEngineVersion,
                      .mCopyright = kEngineCopyright,
                      .mStatus = "Creating window...",
                      .mProgress = 0.0f });
    }

    App::App(const AppDesc &desc)
        : mDesc(Prepared(desc)),
          mProject(mDesc.mProject),
          mSplash(mDesc),
          mCore(Context(), mCommands),
          mAssets(CookedAssetsDir()),
          mScene(Context(), mProject.mSceneDir),
          mPrefab(Context(), PrefabSourceDir(mProject)),
          mWindow(MakeWindowDesc(mDesc)),
          mRenderer(Context(), mWindow.Get(), mAssets,
              RendererModuleSettings{ .mAppName = mDesc.mAppName,
                                      .mEnableValidation = mDesc.mEnableValidation,
                                      .mProjectSettingsPath = mProject.mProjectSettingsPath,
                                      .mAssetIdRoot = mProject.mAssetIdRoot }),
          mImGui(Context(), mWindow, mRenderer),
    #if MIR_WITH_EDITOR
          mEditor(Context(), mImGui, mWindow, mRenderer, mScene, mAssets, mProject, mDesc.mEnableEditorLayout),
    #endif
          mInput(Context(), mWindow.Get(), mProject.mProjectSettingsPath),
          mCamera(Context()),
          mAnimation(Context(), mRenderer.Renderer(), mAssets, mProject.mAssetIdRoot),
          mAudio(Context(), mProject.mSoundBankDir),
          mPhysics(Context()),
          mTerrainData(Context()),
          mScript(Context(), mAssets, mProject.mAssetIdRoot),
          mAudioScript(mScript),
          mInputScript(mScript),
          mPhysicsScript(mScript),
          mRendererScript(mScript),
          mTerrainScript(mScript),
          mTerrain(Context(), mRenderer, mProject),
          mFixedTimestep{ .mStep = mDesc.mFixedDeltaSeconds, .mMaxSteps = mDesc.mMaxFixedSteps }
    {
        // after the editor's drawer, so the game UI fits this frame's scene viewport
        mUiDrawer = mImGui.AddDrawer([this] { DrawUI(); });

        if (mDesc.mUIDemo)
            mUIDemo.Load(mGameUI, Assets());

        mSplash.screen.SetProgress("Loading scene...", 0.9f);
        mPrefab.Coordinator().Bind(mScene.Scene());
        LoadProjectScene();

        //without the editor there is no Play button
        if (!mDesc.mEnableEditorLayout)
            PlayScene();

        // every module is built, so a library that took the crash filter slot has done so
        ReclaimCrashHandler();

        mWindow.Get().Show();
    }

    // Systems stop while every module is alive; then members die in reverse
    App::~App()
    {
        SystemContext stopContext = MakeContext(0.0f);
        mScheduler.Stop(stopContext);
        mScheduler.Reset();
        MIR_LOG_INFO("App shut down");
    }

    bool App::RequestOpenProject(const std::string &requestedDir)
    {
        // validate before closing; not ResolveGameProject, which logs "Loading game"
        try
        {
            mProjectToOpen = LoadGameProject(NormalizeGameDir(requestedDir, ErrorCategory::Usage));
            ResolveAssetIdRoot(*mProjectToOpen);
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
        mPrefab.Coordinator().Bind(mScene.Scene());
        ClearHistory();
        SetSceneUnsaved(false);
    }

    bool App::SaveScene()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::SaveScene: ignored while Play is running (Stop first)");
            return false;
        }

        const bool saved = mScene.Save(&mPrefab.Coordinator().Resolver());
        if (saved)
            SetSceneUnsaved(false);
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
        mPrefab.Coordinator().Bind(mScene.Scene());
        ClearHistory();
        SetSceneUnsaved(false);
        mTerrain.Runtime().Load();
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
                ").\n\nStart with an empty scene? Saving it will overwrite the broken file; restore it from git "
                "first if you need it.\n\n" + refuse;
            return AskYesNo("Scene file is corrupt", message, mDesc.mNonInteractive) ? SceneLoadChoice::StartEmpty
                                                                                     : SceneLoadChoice::Refuse;
        }

        const std::string message = std::to_string(MissingFileCount(probe)) + " file(s) listed in " + manifest +
            " are missing or unreadable (" + MissingFileList(probe) + ").\n\nOpen the scene without them? Saving it will drop them for good; restore them from git first "
            "if you need them.\n\n" + refuse;
        return AskYesNo("Scene has missing entities", message, mDesc.mNonInteractive) ? SceneLoadChoice::Load
                                                                                        : SceneLoadChoice::Refuse;
    }

    void App::PlayScene()
    {
        if (IsPlaying(mWorld))
            return;

        // Play pins the terrain releases present on entry
        mTerrain.Runtime().Refresh(0.0f, true);
        mTerrain.Runtime().BeginPlay();

        mScene.CaptureForPlay();
        mUnsavedBeforePlay = SceneUnsaved();
        ClearHistory();

        SetPlayState(mWorld, PlayState::Running);
        SetEngineMode(mWorld, EngineMode::Play);
    }

    void App::StopScene()
    {
        if (!IsPlaying(mWorld))
            return;

        SystemContext context = MakeContext(0.0f);
        mScript.Scripts().StopAllScripts(context);

        mScene.RestoreFromPlay();
        mPrefab.Coordinator().Bind(mScene.Scene());
        ClearHistory();

        //the World is back as it was at Play, Play's edits gone, so the marker goes back too
        SetSceneUnsaved(mUnsavedBeforePlay);

        // Play's entities are gone and the restored ones hold no handles yet
        mAnimation.Models().Reset();
        mRenderer.Meshes().Reset();
        mWorld.Resource<ModelDrawItems>().items.clear();
        mWorld.Resource<ModelMorphOverrides>().instances.clear();
        mTerrain.Runtime().ForgetMeshes();
        mRenderer.Renderer().ReleaseSceneResources();

        SetEngineMode(mWorld, EngineMode::Edit);
        SetPlayState(mWorld, PlayState::Running);

        // Starts checking a release published during Play
        mTerrain.Runtime().Refresh(0.0f, true);
    }

    void App::TogglePause()
    {
        if (!IsPlaying(mWorld))
            return;

        SetPlayState(mWorld, IsPaused(mWorld) ? PlayState::Running : PlayState::Paused);
    }

    void App::ApplyShellAction(GameShellAction action)
    {
        RequestQueue &requests = mWorld.Resource<RequestQueue>();
        switch (action)
        {
        case GameShellAction::Pause:
            requests.Push(Request::Pause);
            break;
        case GameShellAction::Resume:
            requests.Push(Request::Resume);
            break;
        case GameShellAction::Restart:
            requests.Push(Request::Stop);
            requests.Push(Request::Play);
            break;
        case GameShellAction::Quit:
            //the editor's Quit leaves Play, but game build closes
            requests.Push(mDesc.mEnableEditorLayout ? Request::Stop : Request::Quit);
            break;
        case GameShellAction::None:
            break;
        }
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

    void App::ClearHistory()
    {
    #if MIR_WITH_EDITOR
        mEditor.Get().ClearHistory();
    #endif
    }

    bool App::SceneUnsaved() const
    {
    #if MIR_WITH_EDITOR
        return mEditor.Get().IsSceneUnsaved();
    #else
        return false;
    #endif
    }

    void App::SetSceneUnsaved([[maybe_unused]] bool unsaved)
    {
    #if MIR_WITH_EDITOR
        mEditor.Get().SetSceneUnsaved(unsaved);
    #endif
    }

    void App::UnloadCurrentScene()
    {
        mScene.Unload();

        // no scene entity holds a mesh or material handle now
        mAnimation.Models().Reset();
        mRenderer.Meshes().Reset();
        if (auto *draws = mWorld.TryResource<ModelDrawItems>())
            draws->items.clear();
        if (auto *overrides = mWorld.TryResource<ModelMorphOverrides>())
            overrides->instances.clear();
        mTerrain.Runtime().Unload();
        mRenderer.Renderer().ReleaseSceneResources();
    }

    void App::LoadProjectScene()
    {
        const std::filesystem::path manifest = SceneManifestPath(mProject.mSceneDir);

        std::error_code ec;
        if (!std::filesystem::exists(manifest, ec))
        {
            MIR_LOG_INFO("No {} in '{}', starting with an empty scene", kSceneManifestFileName, mProject.mSceneDir.string());
            return;
        }

        const SceneProbe probe = ProbeScene(mProject.mSceneDir);
        switch (ChooseSceneLoad(probe, true))
        {
        case SceneLoadChoice::StartEmpty:
            MIR_LOG_WARN("Starting with an empty scene over the corrupt '{}'", manifest.string());
            mScene.New("untitled", true);
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

        while (!mWindow.Get().ShouldClose())
        {
            mWindow.Get().PollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), mDesc.mMaxDeltaSeconds);
            previous = now;
            mElapsed += dt;

            // nothing is drawn or running yet, so a transition frees nothing in use
            ApplyRequests();

            SystemContext frameContext = MakeContext(dt);
            mScheduler.RunFrameSlot(FrameSlot::FrameBegin, frameContext);

            mFrameDt = dt;
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

    void App::DrawUI()
    {
        if (mWindow.Get().Width() == 0 || mWindow.Get().Height() == 0)
            return;

        mGameUI.BeginFrame(GameUIArea(), mWorld.Resource<InputState>(), mWorld.Resource<InputMap>(),
            ToUIPointer(mWindow.Get().RawInput()));
        GameShellAction shellAction = GameShellAction::None;
        if (mDesc.mUIDemo)
            mUIDemo.Draw(mGameUI);
        else
            shellAction = mGameShell.Draw(mGameUI, IsPlaying(mWorld), mFrameDt);
        mGameUI.EndFrame();

        ApplyShellAction(shellAction);
    }

    UIRect App::GameUIArea() const
    {
    #if MIR_WITH_EDITOR
        if (mDesc.mEnableEditorLayout)
            return ToUIRect(mEditor.Get().SceneViewportRect());
    #endif
        return UIRect{ 0.0f, 0.0f, static_cast<float>(mWindow.Get().Width()), static_cast<float>(mWindow.Get().Height()) };
    }

#if MIR_WITH_EDITOR
    void App::ApplyImportedAssets()
    {
        try
        {
            mAssets.BeginReload();
        }
        catch (const std::exception &error)
        {
            mEditor.Import().MarkApplyFailed(error.what());
            return;
        }

        std::string error;
        if (!mAnimation.Models().PrepareReload(mAssets.Candidate(), error))
        {
            mAssets.AbortReload();
            mEditor.Import().MarkApplyFailed("new model resources were rejected: " + error);
            return;
        }

        mAssets.CommitReload();
        mAnimation.Models().CommitReload(&mAssets.Cache());
        mRenderer.Meshes().CommitReload(&mAssets.Cache());

        // a script whose asset arrived with this cook resolves now
        mScript.Resolver().ForgetFailures();
        mEditor.Import().MarkApplied();
        MIR_LOG_INFO("Applied imported assets at an Edit frame boundary");
    }
#endif
} // namespace mir
