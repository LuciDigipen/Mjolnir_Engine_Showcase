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
#include <core/ecs/SceneLoadGroup.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <core/platform/CrashHandler.h>
#if MIR_WITH_EDITOR
#include <editor/Selection.h>
#endif
#include <input/DefaultInputMap.h>
#include <input/InputState.h>
#include <prefab/PrefabCoordinator.h>
#include <project/ProjectSettings.h>
#include <scene/SceneIO.h>
#include <serial/JsonFile.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <exception>
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

    App::WorldState::WorldState(const GameProject &project, AssetsModule &assets, const AssetResolver &resolver,
        GameShellSettings shellSettings)
        : particles(Context(), assets, resolver),
        scene(Context(), project.mSceneDir),
        prefab(Context(), scene, PrefabSourceDir(project), kHasEditor ? nullptr : &assets),
        gameUI(Context()),
        gameShell(Context(), gameUI, assets, resolver, shellSettings)
    {}

    App::SimulationSystems::SimulationSystems(ModuleContext context, const GameProject &project, JobExecutor &jobs,
        AssetsModule &assets, const AssetResolver &resolver, ParticlesModule &particles,
        ParticlesGpuModule &particlesGpu, RenderSystem &rendering)
        : physics(context, project.mProjectSettingsPath, &jobs),
        terrainData(context),
        script(context, assets, resolver),
        particlesRuntime(context, particles, particlesGpu, rendering),
        particlesScript(script),
    #if MIR_WITH_AUDIO
        audioScript(script),
    #endif
        inputScript(script),
        physicsScript(script),
        rendererScript(script),
        terrainScript(script)
    {}

    struct App::RuntimeSession
    {
        explicit RuntimeSession(App &app)
            : state(app.mProject, app.mAssets, app.mResolver,
                GameShellSettings{ .mUIDemo = app.mDesc.mUIDemo, .mQuitStopsPlay = app.mDesc.mEnableEditorLayout }),
            rendering(app.mRenderer.BindWorld(state.Context(),
                RendererModuleSettings{ .mAppName = app.mDesc.mAppName,
                                        .mEnableValidation = app.mDesc.mEnableValidation,
                                        .mProjectSettingsPath = app.mProject.mProjectSettingsPath,
                                        .mResolver = &app.mResolver,
                                        .mAssetService = app.mAssetService.get(),
                                        .mAdvanceSharedFrame = false })),
            input(state.Context(), app.mWindow.Get(), app.mProject.mProjectSettingsPath),
            animation(state.Context(), app.mRenderer.Renderer(), app.mAssetService.get(), app.mResolver, &app.mJobs),
            camera(state.Context()),
        #if MIR_WITH_AUDIO
            audio(app.mAudio.BindWorld(state.Context())),
        #endif
            simulation(state.Context(), app.mProject, app.mJobs, app.mAssets, app.mResolver, state.particles,
                app.mParticlesGpu, rendering->Rendering()),
            terrain(state.Context(), app.mRenderer, app.mProject, app.mResolver, app.mJobs)
        {
        #if MIR_WITH_AUDIO
            state.gameShell.SetAudioOptions(&app.mAudioOptions);
        #endif
        #if MIR_WITH_EDITOR
            state.world.EmplaceResource<Selection>();
        #endif
            state.prefab.Coordinator().CopyPrefabDefinitionsFrom(app.mWorldState.prefab.Coordinator());
            state.scene.CopyFrom(app.mWorldState.world, app.mWorldState.scene.Scene(),
                &app.mWorldState.prefab.Coordinator().Resolver(), &state.prefab.Coordinator().Resolver());
            SetEngineMode(state.world, EngineMode::Play);
            Begin(0.0, 0);
        }

        void Begin(double elapsed, uint64_t frame)
        {
            SystemContext context = MakeContext(0.0f, elapsed, frame, 0.0f);
            state.scheduler.RunStage(Stage::SceneLoad, context);
            state.scheduler.RunStage(Stage::PlayBegin, context);
        }

        SystemContext MakeContext(float dt, double elapsed, uint64_t frame, float alpha)
        {
            return SystemContext{ state.world, state.commands, dt, elapsed, frame, alpha, state.scene.Scene().mInstanceId };
        }

        WorldState state;
        std::unique_ptr<RendererModule::WorldBinding> rendering;
        InputModule input;
        AnimationModule animation;
        CameraModule camera;
    #if MIR_WITH_AUDIO
        std::unique_ptr<AudioModule::WorldBinding> audio;
    #endif
        SimulationSystems simulation;
        TerrainRuntimeModule terrain;
    };

    App::App(const AppDesc &desc)
        : mDesc(Prepared(desc)),
        mProject(mDesc.mProject),
        mSplash(mDesc),
        mJobs(mDesc.mWorkerThreads),
        mAssets(CookedAssetsDir()),
        mAssetService(MakeAssetService(mAssets, mJobs, mProject.mProjectSettingsPath)),
        mResolver(mAssets, mProject.mAssetIdRoot),
        mWorldState(mProject, mAssets, mResolver,
            GameShellSettings{ .mUIDemo = mDesc.mUIDemo, .mQuitStopsPlay = mDesc.mEnableEditorLayout }),
        mWindow(MakeWindowDesc(mDesc)),
        mRenderer(Context(), mWindow.Get(),
            RendererModuleSettings{ .mAppName = mDesc.mAppName,
                                    .mEnableValidation = mDesc.mEnableValidation,
                                    .mProjectSettingsPath = mProject.mProjectSettingsPath,
                                    .mResolver = &mResolver,
                                    .mAssetService = mAssetService.get() }),
        mParticlesGpu(mRenderer.Renderer()),
        mInput(Context(), mWindow.Get(), mProject.mProjectSettingsPath),
        mAnimation(Context(), mRenderer.Renderer(), mAssetService.get(), mResolver, &mJobs),
        mImGui(Context(), mWindow, mRenderer),
    #if MIR_WITH_EDITOR
        mEditor(Context(), mImGui, mWindow, mRenderer, mWorldState.scene, mWorldState.prefab, mAssets, mAssetService.get(), mResolver,
            mProject, mJobs, mDesc.mEnableEditorLayout, mAnimation.Library()),
    #endif
        mCamera(Context()),
    #if MIR_WITH_AUDIO
        mAudio(Context(), mProject.mSoundBankDir, mProject.mProjectSettingsPath,
            UserDataDir() / mProject.mName / "settings.json"),
    #endif
        mSimulation(Context(), mProject, mJobs, mAssets, mResolver, mWorldState.particles,
            mParticlesGpu, mRenderer.Rendering()),
        mTerrain(Context(), mRenderer, mProject, mResolver, mJobs, false),
        mGameUIImGui(Context(), mImGui, mWindow, mWorldState.gameUI),
        mFixedTimestep{ .mStep = mDesc.mFixedDeltaSeconds, .mMaxSteps = mDesc.mMaxFixedSteps }
    {
    #if MIR_WITH_AUDIO
        mWorldState.gameShell.SetAudioOptions(&mAudioOptions);
    #if MIR_WITH_EDITOR
        mEditor.SetAudio(&mEditorAudio);
    #endif
    #endif
    #if MIR_WITH_EDITOR
        mEditor.SetNonInteractive(mDesc.mNonInteractive);
        mEditor.Get().SetSceneCatalog(mProject.mScenes, mProject.mStartupSceneId);
    #endif

        mSplash.screen.SetProgress("Loading scene...", 0.9f);
        LoadProjectScene();

        //without the editor there is no Play button
        if (!mDesc.mEnableEditorLayout)
        {
            mGateScenes = true;
            mLoadingDrawer = mImGui.AddDrawer(DrawerGroup::Game, [this] { DrawLoadingOverlay(); });
            PlayScene();
        }

        // every module is built, so a library that took the crash filter slot has done so
        ReclaimCrashHandler();

        if (!mDesc.mHidden) mWindow.Get().Show();
    }

    // Systems stop while every module is alive; then members die in reverse, the
    // modules before the scheduler, so a module's system pointers stay valid to the end
    App::~App()
    {
        StopScene();
        SystemContext stopContext = MakeContext(0.0f);
        mWorldState.scheduler.Stop(stopContext);
        MIR_LOG_INFO("App shut down");
    }

    World *App::RuntimeWorld() { return mRuntime ? &mRuntime->state.world : nullptr; }

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
        if (IsPlaying(mWorldState.world))
        {
            MIR_LOG_WARN("App::NewScene: ignored while Play is running (Stop first)");
            return;
        }

        UnloadCurrentScene();
        mWorldState.scene.New(std::move(name), incomplete);
        RunStage(Stage::SceneLoad);
    }

    bool App::SaveScene()
    {
        if (IsPlaying(mWorldState.world))
        {
            MIR_LOG_WARN("App::SaveScene: ignored while Play is running (Stop first)");
            return false;
        }

        if (!mWorldState.scene.IsBound())
        {
            if (!ConfirmBindUntitledScene())
            {
                MIR_LOG_WARN("App::SaveScene: the untitled scene was not saved, '{}' is left as it is",
                    mWorldState.scene.SceneDir().string());
                return false;
            }
            mWorldState.scene.Bind();
        }

        const bool saved = mWorldState.scene.Save(&mWorldState.prefab.Coordinator().Resolver());
        if (saved)
            RunStage(Stage::SceneSaved);
        return saved;
    }

    bool App::LoadScene()
    {
        return OpenScene(mOpenSceneId, false);
    }

#if MIR_WITH_EDITOR
    void App::CreateScene(const std::string &id, const std::string &path, bool saveCurrent)
    {
        namespace fs = std::filesystem;
        const auto fail = [this](std::string message)
            {
                MIR_LOG_WARN("Create Scene: {}", message);
                mEditor.Get().SetSceneCreationError(std::move(message));
            };
        if (!IsEditing(mWorldState.world))
            return;
        if (id.empty() || id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") !=
            std::string::npos)
            return fail("Use letters, numbers, underscores or hyphens for the scene name.");

        const fs::path relative = fs::u8path(path).lexically_normal();
        if (relative.empty() || relative == "." || relative.has_root_path() || *relative.begin() == "..")
            return fail("Choose a folder inside the game directory.");

        std::error_code ec;
        const fs::path root = fs::canonical(mProject.mRootDir, ec);
        if (ec)
            return fail("Could not resolve the game directory.");
        const fs::path directory = root / relative;
        const fs::path resolved = fs::weakly_canonical(directory, ec).lexically_relative(root);
        if (ec || resolved.empty() || resolved == "." || *resolved.begin() == "..")
            return fail("Choose a folder inside the game directory.");

        const fs::path manifestPath = root / kGameManifestFileName;
        auto manifest = ReadJsonFile(manifestPath);
        if (!manifest || !manifest->is_object())
            return fail("Could not read game.json.");

        if (!manifest->contains("scenes"))
        {
            const auto startup = mProject.mSceneDir.lexically_relative(mProject.mRootDir);
            if (startup.empty() || startup == "." || *startup.begin() == "..")
                return fail("Move the startup scene inside the game directory before registering more scenes.");
            (*manifest)["startupScene"] = mProject.mStartupSceneId;
            (*manifest)["scenes"] = nlohmann::json::array({
                { { "id", mProject.mStartupSceneId },{ "path", startup.generic_string() } }
                });
        }
        auto &scenes = (*manifest)["scenes"];
        if (!scenes.is_array())
            return fail("The scene catalog in game.json is invalid.");
        for (const auto &entry : scenes)
        {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
                !entry.contains("path") || !entry["path"].is_string())
                return fail("The scene catalog in game.json is invalid.");
            if (entry["id"] == id)
                return fail("That scene name is already registered.");
            if (fs::u8path(entry["path"].get<std::string>()).lexically_normal() == relative)
                return fail("That folder is already registered.");
        }

        if (fs::exists(directory, ec) || ec)
            return fail("Choose a new folder. Existing folders are kept intact.");
        if (saveCurrent && !SaveScene())
            return fail("Could not save the current scene.");
        if (!fs::is_regular_file(SceneManifestPath(mProject.mSceneDir), ec))
            return fail("Save the startup scene before creating another scene.");

        fs::create_directories(directory.parent_path(), ec);
        if (ec || !fs::create_directory(directory, ec))
            return fail("Could not create the new scene folder.");

        const auto utf8Path = relative.generic_u8string();
        scenes.push_back({ { "id", id },{ "path", std::string(utf8Path.begin(), utf8Path.end()) } });
        if (!mir::SaveScene(mWorldState.world, directory, mir::NewScene(id), nullptr) ||
            !WriteJsonFile(manifestPath, *manifest))
        {
            fs::remove(SceneManifestPath(directory), ec);
            fs::remove(directory / "entities", ec);
            fs::remove(directory, ec);
            return fail("Could not write the new scene or update game.json.");
        }

        mProject.mScenes.push_back({ id, mProject.mRootDir / relative });
        mEditor.Get().SetSceneCatalog(mProject.mScenes, mOpenSceneId);
        OpenScene(id, false);
    }
#endif

    bool App::OpenScene(const std::string &id, bool saveCurrent)
    {
        if (IsPlaying(mWorldState.world))
        {
            MIR_LOG_WARN("App::LoadScene: ignored while Play is running (Stop first)");
            return false;
        }

        const auto target = std::ranges::find(mProject.mScenes, id, &ProjectScene::mId);
        if (target == mProject.mScenes.end())
            return false;
        const auto &directory = target->mDirectory;
        if (!std::filesystem::exists(SceneManifestPath(directory)))
        {
            MIR_LOG_ERROR("App::LoadScene: no {} in '{}'", kSceneManifestFileName, directory.string());
            return false;
        }

        // probed before unloading, so refusing keeps the current scene intact
        const SceneProbe probe = ProbeScene(directory);
        const SceneLoadChoice choice = ChooseSceneLoad(probe, directory, false);
        switch (choice)
        {
        case SceneLoadChoice::Refuse:
            MIR_LOG_WARN("App::LoadScene: kept the current scene over the broken one in '{}'",
                directory.string());
            return false;
        case SceneLoadChoice::StartEmpty:
        case SceneLoadChoice::Load:
            break;
        }

        if (saveCurrent && !SaveScene())
            return false;

        UnloadCurrentScene();
        mWorldState.scene.SetSceneDir(directory);
        mOpenSceneId = id;
    #if MIR_WITH_EDITOR
        mEditor.Get().SetOpenSceneId(id);
    #endif
        if (choice == SceneLoadChoice::StartEmpty)
            mWorldState.scene.New("untitled", true);
        else
            mWorldState.scene.LoadFromDisk(&mWorldState.prefab.Coordinator().Resolver(), !probe.Ok());
        RunStage(Stage::SceneLoad);
        return true;
    }

    void App::ReplaceSceneFromDisk(bool incomplete)
    {
        UnloadCurrentScene();
        mWorldState.scene.LoadFromDisk(&mWorldState.prefab.Coordinator().Resolver(), incomplete);
        RunStage(Stage::SceneLoad);
    }

    App::SceneLoadChoice App::ChooseSceneLoad(const SceneProbe &probe, const std::filesystem::path &directory, bool atStartup) const
    {
        if (probe.Ok())
            return SceneLoadChoice::Load;

        // a game build has nobody who could repair the scene, so it never offers to
        if (!kHasEditor)
            return SceneLoadChoice::Refuse;

        const std::string refuse = atStartup ? "Choose No to quit." : "Choose No to keep the current scene.";
        const std::string manifest = SceneManifestPath(directory).string();

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
        const std::string sceneDir = mWorldState.scene.SceneDir().string();
        const std::string message = "This scene is untitled. It was started empty in place of the scene in " +
            sceneDir + ", which could not be read.\n\nSave it there? That replaces " +
            std::string(kSceneManifestFileName) + " and every entity and instance file in " + sceneDir +
            "; restore them from git first if you need them. To open the old scene instead, restore it and "
            "choose File > Load Scene.\n\nChoose No to leave the files as they are.";
        return AskYesNo("Save the untitled scene", message, mDesc.mNonInteractive);
    }

    void App::PlayScene()
    {
        if (IsPlaying(mWorldState.world))
            return;

        // Commit prefab edits before copying the live authoring scene into Play.
        PrefabCoordinator &prefabs = mWorldState.prefab.Coordinator();
        prefabs.Commit(mWorldState.world);
        prefabs.SetFrozen(true);
    #if MIR_WITH_EDITOR
        mEditor.PreparePlay();
    #endif
        mRuntime = std::make_unique<RuntimeSession>(*this);
        if (mWorldState.scheduler.Started())
        {
            SystemContext start = MakeRuntimeContext(0.0f);
            mRuntime->state.scheduler.Start(start);
        }
        SetPlayState(mWorldState.world, PlayState::Running);
        SetEngineMode(mWorldState.world, EngineMode::Play);
        mRenderer.ActivateWorld(mRuntime->rendering.get());
        mGameUIImGui.ActivateWorld(&mRuntime->state.world, &mRuntime->state.gameUI);
        mImGui.SetInputWorld(&mRuntime->state.world);
    #if MIR_WITH_EDITOR
        mEditor.SetPlayWorld(&mRuntime->state.world);
    #endif
    }

    void App::StopScene()
    {
        mSceneLoader.Cancel();
        if (!IsPlaying(mWorldState.world))
            return;

        SystemContext runtimeContext = MakeRuntimeContext(0.0f);
        if (mRuntime->state.scheduler.Started())
            mRuntime->state.scheduler.RunStage(Stage::PlayEnd, runtimeContext);
        mRuntime->state.scene.Unload();
        mRuntime->state.scheduler.RunStage(Stage::SceneUnload, runtimeContext);
        mRuntime->state.scheduler.Stop(runtimeContext);

    #if MIR_WITH_EDITOR
        mEditor.SetPlayWorld(nullptr);
    #endif
        mGameUIImGui.ActivateWorld(nullptr, nullptr);
        mImGui.SetInputWorld(nullptr);
        mRenderer.ActivateWorld(nullptr);
        mRuntime.reset();

    #if MIR_WITH_EDITOR
        mEditor.FinishPlay();
    #endif
        mWorldState.prefab.Coordinator().SetFrozen(false);

        SetEngineMode(mWorldState.world, EngineMode::Edit);
        SetPlayState(mWorldState.world, PlayState::Running);
    }

    bool App::SwapScene(const std::string &id)
    {
        mSceneLoader.Cancel();
        if (!mRuntime)
            return false;

        const auto target = std::ranges::find(mProject.mScenes, id, &ProjectScene::mId);
        if (target == mProject.mScenes.end())
        {
            MIR_LOG_ERROR("Scene swap: unknown scene ID '{}'", id);
            return false;
        }

        mSceneLoader.Request(id, target->mDirectory, mRuntime->state.scene.Scene().mInstanceId);
        return true;
    }

    void App::PollSceneLoad()
    {
        if (!mRuntime || mWindow.Get().ShouldClose())
        {
            mSceneLoader.Cancel();
            return;
        }
        if (const auto completed = mSceneLoader.TakeReady(mRuntime->state.scene.Scene().mInstanceId))
            ActivateScene(*completed);
    }

    bool App::ActivateScene(const SceneLoadCompletion &completed)
    {
        if (!mRuntime || completed.source != mRuntime->state.scene.Scene().mInstanceId)
            return false;
        const std::string &id = completed.id;
        const SceneProbe &probe = completed.prepared.probe;
        if (!completed.error.empty())
        {
            MIR_LOG_ERROR("Scene swap: scene '{}' could not be prepared ({})", id, completed.error);
            return false;
        }
        if (!probe.Ok())
        {
            MIR_LOG_ERROR("Scene swap: scene '{}' could not be prepared ({}, {} missing files)", id,
                probe.mManifestError, MissingFileCount(probe));
            return false;
        }

        World &world = mRuntime->state.world;
        LoadedScene prepared;
        try
        {
            prepared = mir::LoadPreparedScene(world, completed.prepared,
                &mRuntime->state.prefab.Coordinator().Resolver(), prepared.mInstanceId);
            if (!prepared.mComplete)
            {
                mir::UnloadScene(world, prepared);
                world.ClearSceneResources(prepared.mInstanceId);
                MIR_LOG_ERROR("Scene swap: scene '{}' could not be prepared completely", id);
                return false;
            }
        }
        catch (const std::exception &error)
        {
            mir::UnloadScene(world, prepared);
            world.ClearSceneResources(prepared.mInstanceId);
            MIR_LOG_ERROR("Scene swap: scene '{}' could not be prepared ({})", id, error.what());
            return false;
        }

        SystemContext oldContext = MakeRuntimeContext(0.0f);
        oldContext.sceneSwapped = true;
        mRuntime->state.scheduler.RunStage(Stage::SceneEnd, oldContext);
        mRuntime->state.scene.Unload();
        mRuntime->state.scheduler.RunStage(Stage::SceneUnload, oldContext);
        mRuntime->state.scene.Adopt(std::move(prepared), completed.directory);
        SystemContext loadedContext = MakeRuntimeContext(0.0f);
        loadedContext.sceneSwapped = true;
        mRuntime->state.scheduler.RunStage(Stage::SceneLoad, loadedContext);
        mRenderer.ActivateWorld(mRuntime->rendering.get());
        mGameUIImGui.ActivateWorld(&mRuntime->state.world, &mRuntime->state.gameUI);
        mImGui.SetInputWorld(&mRuntime->state.world);
    #if MIR_WITH_EDITOR
        mEditor.SetPlayWorld(&mRuntime->state.world);
    #endif
        MIR_LOG_INFO("Scene swap: activated '{}'", id);
        return true;
    }

    void App::TogglePause()
    {
        if (!IsPlaying(mWorldState.world))
            return;

        const PlayState state = IsPaused(mWorldState.world) ? PlayState::Running : PlayState::Paused;
        SetPlayState(mWorldState.world, state);
        SetPlayState(mRuntime->state.world, state);
    }

    bool App::AwaitSceneResources()
    {
        WorldState &active = mRuntime ? mRuntime->state : mWorldState;
        SceneLoadGroup &group = active.world.Resource<SceneLoadGroup>();
        mSceneGated = true;
        // The game UI stays off while the scene prepares, so nothing it draws can be clicked, and what is requested
        // meanwhile is dropped. A request made before the wait is kept.
        active.gameUI.SetSuspended(true);
        RequestQueue &requests = active.world.Resource<RequestQueue>();
        RequestQueue::Held held = requests.Hold();
        const auto finish = [&]
            {
                mSceneGated = false;
                active.gameUI.SetSuspended(false);
                if (const size_t dropped = requests.Restore(std::move(held)); dropped != 0)
                    MIR_LOG_WARN("Dropped {} request(s) made while the scene was loading", dropped);
            };
        uint64_t frames = 0;
        // Wait for resources without starting gameplay. Background I/O sets the pace, so a limited run is bound
        // by time and not by its frame limit.
        constexpr std::chrono::seconds kLoadingTimeout{ 60 };
        const auto loadingStart = std::chrono::steady_clock::now();
        while (!group.Ready())
        {
            if (mWindow.Get().ShouldClose())
            {
                finish();
                return false;
            }
            mWindow.Get().PollEvents();
            if (mAssetService)
                mAssetService->DrainCompletions();
            mImGui.RunUI();
            SystemContext context = mRuntime ? MakeRuntimeContext(0.0f) : MakeContext(0.0f);
            active.scheduler.UpdateLoading(context);
            group.EndFrame();
            ++frames;
            if (!group.Ready() && mDesc.mMaxFrames != 0 &&
                std::chrono::steady_clock::now() - loadingStart >= kLoadingTimeout)
            {
                MIR_LOG_ERROR("The scene was still not ready after {} loading frames and {} seconds, gameplay did not start",
                    frames, kLoadingTimeout.count());
                mWindow.Get().RequestClose();
                finish();
                return false;
            }
        }
        finish();
        MIR_LOG_INFO("Scene ready after {} loading frames", frames);
        return true;
    }

    void App::DrawLoadingOverlay()
    {
        if (!mSceneGated)
            return;
        const WorldState &active = mRuntime ? mRuntime->state : mWorldState;
        const SceneLoadGroup &group = active.world.Resource<SceneLoadGroup>();
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        ImDrawList *draw = ImGui::GetForegroundDrawList();
        const ImVec2 min = viewport->Pos, max(min.x + viewport->Size.x, min.y + viewport->Size.y);
        draw->AddRectFilled(min, ImVec2(max.x, max.y), IM_COL32(0, 0, 0, 255));

        // Do not show a percentage until the required resources are known.
        const char *status = group.Discovered() ? "Loading scene..." : "Preparing scene...";
        const float width = std::min(viewport->Size.x * 0.4f, 480.0f);
        const ImVec2 center(min.x + viewport->Size.x * 0.5f, min.y + viewport->Size.y * 0.5f);
        const ImVec2 textSize = ImGui::CalcTextSize(status);
        draw->AddText(ImVec2(center.x - textSize.x * 0.5f, center.y - 24.0f), IM_COL32(255, 255, 255, 255), status);
        draw->AddRect(ImVec2(center.x - width * 0.5f, center.y), ImVec2(center.x + width * 0.5f, center.y + 8.0f),
            IM_COL32(255, 255, 255, 255));
        draw->AddRectFilled(ImVec2(center.x - width * 0.5f, center.y),
            ImVec2(center.x - width * 0.5f + width * group.Progress(), center.y + 8.0f), IM_COL32(255, 255, 255, 255));
    }

    void App::ApplyRequests()
    {
        RequestQueue &queue = mWorldState.world.Resource<RequestQueue>();
        if (const std::optional<std::string> dir = queue.TakeOpenProject(); dir.has_value())
            RequestOpenProject(*dir);

        if (const auto scene = queue.TakeOpenScene())
        {
        #if MIR_WITH_EDITOR
            if (!scene->newScenePath.empty())
                CreateScene(scene->id, scene->newScenePath, scene->saveCurrent);
            else
            #endif
                OpenScene(scene->id, scene->saveCurrent);
        }

        std::vector<Request> actions = queue.Take();
        std::optional<std::string> nextScene;
        SceneInstanceId sourceScene = kOwnedBySession;
        if (mRuntime)
        {
            sourceScene = mRuntime->state.scene.Scene().mInstanceId;
            RequestQueue &runtimeQueue = mRuntime->state.world.Resource<RequestQueue>();
            nextScene = runtimeQueue.TakeNextScene();
            std::vector<Request> runtimeActions = runtimeQueue.Take();
            actions.insert(actions.end(), runtimeActions.begin(), runtimeActions.end());
        }
        for (const Request request : actions)
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
                if (IsSimulating(mWorldState.world))
                    TogglePause();
                break;
            case Request::Resume:
                if (IsPaused(mWorldState.world))
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
                if (mEditor.Import().PendingApply() && IsEditing(mWorldState.world))
                    ApplyImportedAssets();
            #endif
                break;
            case Request::Quit:
                mWindow.Get().RequestClose();
                break;
            }
        }
        if (nextScene && mRuntime && !mWindow.Get().ShouldClose() &&
            sourceScene == mRuntime->state.scene.Scene().mInstanceId)
            SwapScene(*nextScene);
        PollSceneLoad();
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
        mWorldState.scene.Unload();
        RunStage(Stage::SceneUnload);
    }

    void App::RunStage(Stage stage)
    {
        SystemContext context = MakeContext(0.0f);
        mWorldState.scheduler.RunStage(stage, context);
    }

    void App::LoadProjectScene()
    {
        const std::filesystem::path manifest = SceneManifestPath(mProject.mSceneDir);

        std::error_code ec;
        if (!std::filesystem::exists(manifest, ec))
        {
            MIR_LOG_INFO("No {} in '{}', starting with an empty scene", kSceneManifestFileName, mProject.mSceneDir.string());
            mWorldState.scene.New("untitled");
            RunStage(Stage::SceneLoad);
            return;
        }

        const SceneProbe probe = ProbeScene(mProject.mSceneDir);
        switch (ChooseSceneLoad(probe, mWorldState.scene.SceneDir(), true))
        {
        case SceneLoadChoice::StartEmpty:
            MIR_LOG_WARN("Starting with an untitled empty scene, the corrupt '{}' is left as it is", manifest.string());
            mWorldState.scene.New("untitled", true);
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
        return mWorldState.Context();
    }

    SystemContext App::MakeContext(float dt)
    {
        return SystemContext{ mWorldState.world, mWorldState.commands, dt, mElapsed, mFrame,
            mFixedTimestep.Alpha(), mWorldState.scene.Scene().mInstanceId };
    }

    SystemContext App::MakeRuntimeContext(float dt)
    {
        return mRuntime->MakeContext(dt, mElapsed, mFrame, mFixedTimestep.Alpha());
    }

    void App::Run()
    {
        if (mGateScenes && !(mRuntime ? mRuntime->state.world : mWorldState.world).Resource<SceneLoadGroup>().Ready() &&
            !AwaitSceneResources())
            return;

        SystemContext startContext = MakeContext(0.0f);
        mWorldState.scheduler.Start(startContext);
        if (mRuntime)
        {
            SystemContext runtimeStart = MakeRuntimeContext(0.0f);
            mRuntime->state.scheduler.Start(runtimeStart);
        }

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

            if (mGateScenes && !(mRuntime ? mRuntime->state.world : mWorldState.world).Resource<SceneLoadGroup>().Ready())
            {
                mElapsed -= dt;
                if (!AwaitSceneResources())
                    break;
                previous = std::chrono::steady_clock::now();
                continue;
            }

            SystemContext frameContext = MakeContext(dt);
            mWorldState.scheduler.RunFrameSlot(FrameSlot::FrameBegin, frameContext);
            if (mRuntime)
            {
                SystemContext runtimeFrame = MakeRuntimeContext(dt);
                mRuntime->state.scheduler.RunFrameSlot(FrameSlot::FrameBegin, runtimeFrame);
            }

            mImGui.RunUI();

            // advances in every mode; FixedUpdate systems check IsSimulating
            // themselves, as ScriptSystem does
            const uint32_t fixedSteps = mFixedTimestep.Advance(dt);

            SystemContext context = mRuntime ? MakeRuntimeContext(dt) : MakeContext(dt);
            if (!mDesc.mCapturePath.empty() && mFrame + 1 == mDesc.mMaxFrames)
                mRenderer.Renderer().CaptureNextFrame(mDesc.mCapturePath);
            WorldState &active = mRuntime ? mRuntime->state : mWorldState;
            active.scheduler.Update(context, fixedSteps, mFixedTimestep.mStep);
            active.world.Resource<SceneLoadGroup>().EndFrame();

            if (active.world.Resource<InputState>().JustPressed(kToggleFullscreenAction))
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
