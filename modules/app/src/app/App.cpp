/**
 * @file App.cpp
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <app/App.h>

#include "MeshResolveSystem.h"
#include "ProjectSettings.h"
#include "Prompt.h"
#include "SceneResolve.h"
#include "TerrainRuntime.h"
#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>
#include <assets/AssetPath.h>
#include <audio/AudioEmitterSystem.h>
#include <audio/AudioListenerSystem.h>
#include <audio/AudioSystem.h>
#include <audio/ComponentRegistration.h>
#include <camera/CameraControllerSystem.h>
#include <camera/ComponentRegistration.h>
#include <core/EngineVersion.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/DeferredAccess.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <input/DefaultInputMap.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>
#include <physics/CollisionEventSystem.h>
#include <physics/CollisionEvents.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsInterpolationSystem.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <prefab/PrefabCoordinator.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <renderer/ViewDrawSource.h>
#include <scene/SceneIO.h>
#include <script/ComponentRegistration.h>
#include <script/ScriptSystem.h>
#include <terrain/ComponentRegistration.h>
#include <terrain/TerrainScene.h>

#if MIR_WITH_EDITOR
#include "AppSceneOps.h"
#include "PrefabAuthoringAdapter.h"
#include "PrefabWorkspace.h"
#include <core/platform/Process.h>
#include <editor/IdeSettings.h>
#endif

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <set>
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

    #if MIR_WITH_EDITOR
        PrefabWorkspace::PrefabUnsaved UnsavedPrefabs(const Editor &editor, const PrefabCoordinator &prefabs)
        {
            return [editor = &editor, prefabs = &prefabs](PrefabId id)
                {
                    return !prefabs->Registry().HasSavedFile(id) ||
                        editor->IsDocumentUnsaved(DocumentId{ id.mHigh, id.mLow });
                };
        }

        PrefabWorkspace::PrefabSaved MarkSavedPrefabs(Editor &editor)
        {
            return [editor = &editor](PrefabId id)
                {
                    editor->SetDocumentUnsaved(DocumentId{ id.mHigh, id.mLow }, false);
                };
        }
    #endif
    }

    // Never null; does nothing until Initialize binds it.
    App::App() : mTerrain(std::make_unique<TerrainRuntime>()) {}

    App::~App() { Shutdown(); }

    void App::Initialize(const AppDesc &desc)
    {
        mDesc = desc;
        mProject = desc.mProject;
        mProjectToOpen.reset();
        mSwitchHookFired = false;
        mFixedTimestep = FixedTimestep{ .mStep = desc.mFixedDeltaSeconds, .mMaxSteps = desc.mMaxFixedSteps };

        // A failure anywhere below throws; ~App runs Shutdown, which is safe on whatever got built

        EnsureGameDirectories(mProject);
        ReadProjectSettings(mProject.mProjectSettingsPath);

    #if MIR_WITH_EDITOR
        mAssetImport.Initialize(mProject);
        mContentBrowser.Initialize(mProject);
    #else
            //no editor to lay out, the window is the game
        mDesc.mEnableEditorLayout = false;
        MIR_LOG_INFO("Built without the editor; running the game");
    #endif

        audio::Init(mProject.mSoundBankDir);

        EnsureDpiAware();

        mSplash.Show({ .mEngineName = desc.mAppName,
                       .mVersion = kEngineVersion,
                       .mCopyright = kEngineCopyright,
                       .mStatus = "Creating window...",
                       .mProgress = 0.0f });

        WindowDesc windowDesc{};
        windowDesc.mWidth = desc.mWidth;
        windowDesc.mHeight = desc.mHeight;
        windowDesc.mTitle = mProject.mTitle.c_str();
        windowDesc.mMaximized = mDesc.mEnableEditorLayout;
        windowDesc.mCustomTitleBar = mDesc.mEnableEditorLayout;
        windowDesc.mStartHidden = true;

        mWindow = Window::Create(windowDesc);

        mSplash.SetProgress("Initializing renderer...", 0.2f);
        if (!mRenderer.Initialize(
            { .window = mWindow.get(), .appName = desc.mAppName, .enableValidation = desc.mEnableValidation }))
        {
            throw SetupError(ErrorCategory::System, "The Vulkan renderer could not start", {},
                "the log above names the step that failed and its VkResult",
                "Update the GPU driver, and check that the GPU supports Vulkan.");
        }

        mSplash.SetProgress("Initializing UI...", 0.4f);
        if (!mImGui.Initialize(*mWindow, mRenderer))
        {
            // fonts are copied by the build; any other failure is ImGui's backend, which is ours
            const std::filesystem::path fontsDir = ExecutableDir() / "fonts";
            std::error_code ec;
            if (!std::filesystem::is_directory(fontsDir, ec))
            {
                throw SetupError(ErrorCategory::BuildOutput, "The UI fonts are missing", fontsDir,
                    "the fonts directory next to the engine does not exist", "Rebuild to copy the fonts.");
            }
            throw SetupError(ErrorCategory::Internal, "The UI layer could not start", {},
                "the log above names the ImGui step that failed",
                "This is an engine bug. Report it with the log attached: " + LogFilePath().string());
        }

    #if MIR_WITH_EDITOR
        mSplash.SetProgress("Initializing editor...", 0.5f);
        mSceneOps = std::make_unique<AppSceneOps>(*this);
        if (!mEditor.Initialize(mImGui, *mWindow, mProject.mProjectSettingsPath, mWorld, *mSceneOps,
                mRenderer.SwapchainIsSrgb()))
        {
            throw SetupError(ErrorCategory::Internal, "The editor could not start", {},
                "the log above names the step that failed",
                "This is an engine bug. Report it with the log attached: " + LogFilePath().string());
        }
    #endif

        mInitialized = true;

        LoadAssetManifest();

        mSplash.SetProgress("Registering components...", 0.75f);

        // Before anything may load a script
        RegisterCoreComponents();
        RegisterRendererComponents();
        RegisterRendererEntityTypes();
        RegisterRendererPostEffects();
        RegisterPhysicsComponents();
        RegisterCameraComponents();
        RegisterScriptComponents();
        RegisterAudioComponents();
        RegisterModelPlayer();
        RegisterTerrainComponents();

        mWorld.EmplaceResource<ModelDrawItems>();
        mWorld.EmplaceResource<ModelMorphOverrides>();
        mWorld.EmplaceResource<ViewDrawSources>();
        mWorld.EmplaceResource<TerrainWorld>();

        mWorld.EmplaceResource<EntityChangeListeners>();
        PrefabCoordinator &prefabs = mWorld.EmplaceResource<PrefabCoordinator>();
    #if MIR_WITH_EDITOR
        mPrefabAuthoring = std::make_unique<PrefabAuthoringAdapter>(mWorld, prefabs,
            PrefabSourceDir(mProject));
        mEditor.SetAuthoringBackend(mPrefabAuthoring.get());
        mEditor.SetAuthoredEntities(mPrefabAuthoring.get());
        mPrefabWorkspace = std::make_unique<PrefabWorkspace>(prefabs, *mPrefabAuthoring);
        mPrefabWorkspace->BindResolveSystems(&mRenderer, Assets(), mProject.mAssetIdRoot);
    #endif
        const std::filesystem::path prefabsDir = PrefabSourceDir(mProject);
        std::error_code prefabEc;
        if (std::filesystem::is_directory(prefabsDir, prefabEc))
            prefabs.ScanSources(prefabsDir);

        // defers structural change
        mWorld.EmplaceResource<FrameCommands>(FrameCommands{ &mCommands });

        InputMap inputMap = InputMap::LoadFile(mProject.mProjectSettingsPath).value_or(InputMap{});
        AddDefaultActions(inputMap);
        mWorld.EmplaceResource<InputMap>(std::move(inputMap));

        mWorld.EmplaceResource<RenderSettings>(RenderSettings::LoadFile(mProject.mProjectSettingsPath).value_or(RenderSettings{}));
        mWorld.EmplaceResource<PostProcessingSettings>(
            PostProcessingSettings::LoadFile(mProject.mProjectSettingsPath).value_or(PostProcessingSettings{}));
        mWorld.EmplaceResource<InputState>();
        mWorld.EmplaceResource<EngineModeState>();

        // physcis world - currently Jolt
        mWorld.EmplaceResource<PhysicsWorld>();
        mWorld.EmplaceResource<CollisionEvents>();

        mScheduler.AddSystem<PrefabSystem>(SystemPhase::PreUpdate);
        mScheduler.AddSystem<InputSystem>(SystemPhase::PreUpdate, *mWindow);

        mScriptSystem = &mScheduler.AddSystem<ScriptSystem>(SystemPhase::PreUpdate, mScriptHost);

        // FixedUpdate: once per fixed substep, before PostUpdate turns the
        // Transforms it writes into world matrices
        mScheduler.AddSystem<PhysicsSystem>(SystemPhase::FixedUpdate);


        mScheduler.AddSystem<PhysicsInterpolationSystem>(SystemPhase::PostUpdate);

        //should be before any other system in PostUpdate that reads WorldTransform
        mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate);

        mScheduler.AddSystem<CameraControllerSystem>(SystemPhase::PostUpdate);
        mModelSystem =
            &mScheduler.AddSystem<ModelSystem>(SystemPhase::PostUpdate, mRenderer, Assets(), mProject.mAssetIdRoot);
        mMeshResolveSystem = &mScheduler.AddSystem<MeshResolveSystem>(SystemPhase::PostUpdate, mRenderer, Assets(),
            mProject.mAssetIdRoot);

        // after TransformPropagateSystem: it reads the WorldTransform that system writes
        mScheduler.AddSystem<AudioEmitterSystem>(SystemPhase::PostUpdate);
        mScheduler.AddSystem<AudioListenerSystem>(SystemPhase::PostUpdate);

        mScheduler.AddSystem<CollisionEventSystem>(SystemPhase::PostUpdate);

        mRenderSystem = &mScheduler.AddSystem<RenderSystem>(SystemPhase::Render, mRenderer);

        mTerrain->Bind(mWorld, mRenderer, mProject);

        if (mDesc.mUIDemo)
            mUIDemo.Load(mGameUI, Assets());

        // scanned once: a new game needs a build to cook its assets anyway
        mAvailableProjects = DiscoverGameProjects();
    #if MIR_WITH_EDITOR
        mProjectMenu.clear();
        for (const GameProject &project : mAvailableProjects)
            mProjectMenu.push_back(ProjectMenuEntry{ project.mDir, project.mName });
    #endif

        mSplash.SetProgress("Loading scene...", 0.9f);
        mScene = mir::NewScene("untitled");
        prefabs.Bind(mScene);
        LoadProjectScene();

        //without the editor there is no Play button
        if (!mDesc.mEnableEditorLayout)
            PlayScene();

        mWindow->Show();
    }

    bool App::RequestOpenProject(const std::string &requestedDir)
    {
        if (!mInitialized)
            return false;

        // validate before closing; not ResolveGameProject, which logs "Loading game"
        try
        {
            mProjectToOpen = LoadGameProject(NormalizeGameDir(requestedDir, ErrorCategory::Usage));
        }
        catch (const SetupError &error)
        {
            MIR_LOG_ERROR("App::RequestOpenProject: '{}' cannot be opened ({}: {}), '{}' stays open", requestedDir,
                error.What(), error.Cause(), mProject.mDir);
            return false;
        }

        // one game per process: main.cpp relaunches after Run returns
        MIR_LOG_INFO("Closing '{}' to open '{}'", mProject.mDir, mProjectToOpen->mDir);
        mWindow->RequestClose();
        return true;
    }

    void App::NewScene(std::string name)
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::NewScene: ignored while Play is running (Stop first)");
            return;
        }

        UnloadCurrentScene();
        mScene = mir::NewScene(std::move(name));
        if (PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>())
            prefabs->Bind(mScene);
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

        const PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
        const auto writeScene = [&]()
            {
                return mir::SaveScene(mWorld, mProject.mSceneDir, mScene,
                    prefabs != nullptr ? &prefabs->Resolver() : nullptr);
            };

    #if MIR_WITH_EDITOR
        if (mPrefabWorkspace != nullptr && prefabs != nullptr)
        {
            if (!mPrefabWorkspace->SaveScene(mScene, UnsavedPrefabs(mEditor, *prefabs), MarkSavedPrefabs(mEditor),
                    writeScene))
                return false;
        }
        else if (!writeScene())
        {
            return false;
        }
    #else
        if (!writeScene())
            return false;
    #endif

        mScene.mDirty = false;
        SetSceneUnsaved(false);
        return true;
    }

    bool App::SaveActiveDocument()
    {
        if (IsPlaying(mWorld))
        {
            MIR_LOG_WARN("App::SaveActiveDocument: ignored while Play is running (Stop first)");
            return false;
        }

    #if MIR_WITH_EDITOR
        if (mPrefabWorkspace != nullptr)
        {
            if (const std::optional<PrefabId> active = mPrefabWorkspace->ActiveDocument())
            {
                PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
                if (prefabs == nullptr)
                {
                    MIR_LOG_ERROR("App::SaveActiveDocument: the prefab registry is missing");
                    return false;
                }
                return mPrefabWorkspace->SavePrefab(*active, UnsavedPrefabs(mEditor, *prefabs),
                    MarkSavedPrefabs(mEditor));
            }
        }
    #endif

        return SaveScene();
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
        switch (ChooseSceneLoad(ProbeScene(mProject.mSceneDir), false))
        {
        case SceneLoadChoice::Refuse:
            MIR_LOG_WARN("App::LoadScene: kept the current scene over the broken one in '{}'",
                mProject.mSceneDir.string());
            return false;
        case SceneLoadChoice::StartEmpty:
            NewScene();
            return true;
        case SceneLoadChoice::Load:
            break;
        }

        ReplaceSceneFromDisk();
        return true;
    }

    void App::ReplaceSceneFromDisk()
    {
        UnloadCurrentScene();
        PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
        mScene = mir::LoadScene(mWorld, mProject.mSceneDir, prefabs != nullptr ? &prefabs->Resolver() : nullptr);
        if (prefabs != nullptr)
            prefabs->Bind(mScene);
        ClearHistory();
        SetSceneUnsaved(mScene.mDirty);

        // saved names to script instances
        ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);
        mTerrain->Load();
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

    #if MIR_WITH_EDITOR
        if (mPrefabWorkspace != nullptr)
        {
            const std::optional<PrefabId> active = mPrefabWorkspace->ActiveDocument();
            mPrefabBeforePlay = active ? std::optional<DocumentId>(DocumentId{ active->mHigh, active->mLow })
                                       : std::nullopt;
            mPrefabWorkspace->ActivateScene();
        }
    #endif

        // Play pins the terrain releases present on entry
        mTerrain->Refresh(0.0f, true);
        mTerrain->BeginPlay();

        PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
        if (prefabs != nullptr)
        {
            prefabs->Commit(mWorld);
            if (prefabs->ConsumePublishedChanges())
                ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);
            prefabs->SetFrozen(true);
            if (prefabs->ConsumeSceneMaintenance() && !SceneUnsaved())
                SetSceneUnsaved(true);
        }

        mPlaySnapshot = CaptureWorld(mWorld, mScene, prefabs != nullptr ? &prefabs->Resolver() : nullptr);
        mUnsavedBeforePlay = SceneUnsaved();
    #if MIR_WITH_EDITOR
        if (prefabs != nullptr)
        {
            std::vector<DocumentId> documents;
            for (const PrefabId id : prefabs->DocumentIds())
                documents.push_back(DocumentId{ id.mHigh, id.mLow });
            mPrefabMarksBeforePlay = CapturePrefabPlayMarks(documents,
                [this](DocumentId document) { return mEditor.IsDocumentUnsaved(document); });
        }
    #endif
        ClearHistory();

        SetPlayState(mWorld, PlayState::Running);
        SetEngineMode(mWorld, EngineMode::Play);
    }

    void App::StopScene()
    {
        if (!IsPlaying(mWorld))
            return;

        SystemContext context = MakeContext(0.0f);
        mScriptSystem->StopAllScripts(context);

        PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
        mScene = RestoreWorld(mWorld, mPlaySnapshot, prefabs != nullptr ? &prefabs->Resolver() : nullptr);
        if (prefabs != nullptr)
            prefabs->Bind(mScene);
        ClearHistory();
        if (prefabs != nullptr)
            prefabs->SetFrozen(false);

        //the World is back as it was at Play, Play's edits gone, so the marker goes back too
        SetSceneUnsaved(mUnsavedBeforePlay);
    #if MIR_WITH_EDITOR
        RestorePrefabPlayMarks(mPrefabMarksBeforePlay,
            [this](DocumentId document) { mEditor.SetDocumentUnsaved(document, true); });
    #endif

        // Play's entities are gone and the restored ones hold no handles yet
        mModelSystem->Reset();
        mMeshResolveSystem->Reset();
        mWorld.Resource<ModelDrawItems>().items.clear();
        mWorld.Resource<ModelMorphOverrides>().instances.clear();
        mTerrain->ForgetMeshes();
        mRenderer.ReleaseSceneResources();
    #if MIR_WITH_EDITOR
        if (mPrefabWorkspace != nullptr)
            mPrefabWorkspace->ResetResolve();
    #endif

        ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);

        SetEngineMode(mWorld, EngineMode::Edit);
        SetPlayState(mWorld, PlayState::Running);

    #if MIR_WITH_EDITOR
        if (mPrefabBeforePlay.has_value() && mPrefabWorkspace != nullptr)
        {
            const DocumentId document = *mPrefabBeforePlay;
            if (!mPrefabWorkspace->Open(PrefabId{ document.mHigh, document.mLow }))
                MIR_LOG_WARN("App::StopScene: could not reopen the prefab document");
        }
        mPrefabBeforePlay.reset();
    #endif

        // Starts checking a release published during Play
        mTerrain->Refresh(0.0f, true);
    }

    void App::TogglePause()
    {
        if (!IsPlaying(mWorld))
            return;

        SetPlayState(mWorld, IsPaused(mWorld) ? PlayState::Running : PlayState::Paused);
    }

    void App::ApplyShellAction(GameShellAction action)
    {
        switch (action)
        {
        case GameShellAction::Pause:
            if (IsSimulating(mWorld))
                TogglePause();
            break;
        case GameShellAction::Resume:
            if (IsPaused(mWorld))
                TogglePause();
            break;
        case GameShellAction::Restart:
            StopScene();
            PlayScene();
            break;
        case GameShellAction::Quit:
            //the editor's Quit leaves Play, but game build closes
            if (mDesc.mEnableEditorLayout)
                StopScene();
            else
                mWindow->RequestClose();
            break;
        case GameShellAction::None:
            break;
        }
    }

    void App::ToggleFullscreen()
    {
        if (!mInitialized)
            return;

        mWindow->ToggleFullscreen();

        mRenderer.RequestSwapchainRecreate();
    }

    void App::ClearHistory()
    {
    #if MIR_WITH_EDITOR
        mEditor.ClearHistory();
    #endif
    }

    bool App::SceneUnsaved() const
    {
    #if MIR_WITH_EDITOR
        return mEditor.IsSceneUnsaved();
    #else
        return false;
    #endif
    }

    void App::SetSceneUnsaved([[maybe_unused]] bool unsaved)
    {
    #if MIR_WITH_EDITOR
        mEditor.SetSceneUnsaved(unsaved);
    #endif
    }

    void App::UnloadCurrentScene()
    {
        UnloadScene(mWorld, mScene);

        // no scene entity holds a mesh or material handle now
        if (mModelSystem)
            mModelSystem->Reset();
        if (mMeshResolveSystem)
            mMeshResolveSystem->Reset();
        if (auto *draws = mWorld.TryResource<ModelDrawItems>())
            draws->items.clear();
        if (auto *overrides = mWorld.TryResource<ModelMorphOverrides>())
            overrides->instances.clear();
        mTerrain->Unload();
        mRenderer.ReleaseSceneResources();
    #if MIR_WITH_EDITOR
        if (mPrefabWorkspace != nullptr)
            mPrefabWorkspace->ResetResolve();
    #endif
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
            return;
        case SceneLoadChoice::Load:
            ReplaceSceneFromDisk();
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

    AssetCache *App::Assets()
    {
        return mAssetCache.has_value() ? &*mAssetCache : nullptr;
    }

    void App::LoadAssetManifest()
    {
        const std::filesystem::path manifestPath = CookedAssetsDir() / kAssetManifestFileName;

        // A build with no assets writes no manifest, and that build is still correct
        std::error_code ec;
        const bool exists = std::filesystem::exists(manifestPath, ec);
        if (exists)
        {
            mAssetManifest = AssetManifest::LoadFile(manifestPath);
            if (!mAssetManifest.has_value())
            {
                throw SetupError(ErrorCategory::BuildOutput, "The cooked asset manifest is corrupt", manifestPath,
                    "it could not be parsed as an asset manifest", "Rebuild to re-cook the assets.");
            }
        }
        else
        {
            mAssetManifest.emplace();
            MIR_LOG_INFO("No asset manifest at '{}', starting with no cooked assets", manifestPath.string());
        }

        // after the manifest is engaged, never before: the cache stores a raw
        // pointer to it
        mAssetCache.emplace(&*mAssetManifest, CookedAssetsDir());

    #if MIR_WITH_EDITOR
        // The first frames run without assets; ApplyImportedAssets swaps them in once cooked
        if (!exists && mAssetImport.SourceHasAssets())
            mAssetImport.QueueFullCook();
    #endif
    }

    SystemContext App::MakeContext(float dt)
    {
        return SystemContext{ mWorld, mCommands, dt, mElapsed, mFrame, mFixedTimestep.Alpha() };
    }

    void App::Run()
    {
        if (!mInitialized)
        {
            return;
        }

        SystemContext startContext = MakeContext(0.0f);
        mScheduler.Start(startContext);

        auto previous = std::chrono::steady_clock::now();

        while (!mWindow->ShouldClose())
        {
            mWindow->PollEvents();

            const auto now = std::chrono::steady_clock::now();
            const float dt = std::min(std::chrono::duration<float>(now - previous).count(), mDesc.mMaxDeltaSeconds);
            previous = now;
            mElapsed += dt;

        #if MIR_WITH_EDITOR
            mAssetImport.Update(IsEditing(mWorld));
            if (mAssetImport.PendingApply() && IsEditing(mWorld))
                ApplyImportedAssets();
            mContentBrowser.Update();
            if (mContentBrowser.ConsumePrefabSourceChange())
            {
                if (PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>())
                    prefabs->RescanSources(PrefabSourceDir(mProject));
            }
        #endif

            // Every mode, before any system runs
            mTerrain->Update();

            // Edit, between frames, before any system runs
            if (IsEditing(mWorld))
                mTerrain->Refresh(dt, false);

            mImGui.BeginFrame();

            if (mWindow->Width() != 0 && mWindow->Height() != 0)
            {
            #if MIR_WITH_EDITOR
                World *viewWorld = ActivePreviewWorld();
                const EditorActions actions = mEditor.DrawLayout(
                    mDesc.mEnableEditorLayout, mWorld.Resource<InputMap>(), mWorld.Resource<RenderSettings>(),
                    mWorld.Resource<PostProcessingSettings>(), mWorld.Resource<EngineModeState>(), mProjectMenu,
                    mProject.mDir, viewWorld != nullptr ? *viewWorld : mWorld, mWorld, MakeEditorDocuments(),
                    mAssetImport.View(), mRenderer, MakeContentBrowserView());
                if (mPrefabWorkspace != nullptr)
                    mPrefabWorkspace->CloseMissingDocuments();
            #endif

                mGameUI.BeginFrame(GameUIArea(), mWorld.Resource<InputState>(), mWorld.Resource<InputMap>(),
                    ToUIPointer(mWindow->RawInput()));
                GameShellAction shellAction = GameShellAction::None;
                if (mDesc.mUIDemo)
                    mUIDemo.Draw(mGameUI);
                else
                    shellAction = mGameShell.Draw(mGameUI, IsPlaying(mWorld), dt);
                mGameUI.EndFrame();

                ApplyShellAction(shellAction);

            #if MIR_WITH_EDITOR
                ApplyEditorActions(actions);
            #endif
            }

            mRenderer.SetImGuiDrawData(mImGui.EndFrame());

        #if MIR_WITH_EDITOR
            mRenderSystem->SetViewportRect(0, mDesc.mEnableEditorLayout ? mEditor.SceneViewportRect() : VkRect2D{});

            // Only feed EditorCamera input in Edit.
            const bool editorView = IsEditing(mWorld) && mDesc.mEnableEditorLayout;
            if (editorView)
            {
                mEditor.UpdateCamera(dt);
                const EditorCamera &camera = mEditor.SceneCamera();
                mRenderSystem->SetExternalCamera(0, camera.Pose(), camera.mLens);
            }
            else
            {
                mRenderSystem->SetViewportCamera(0, kNullEntity);
            }
            mRenderSystem->SetSourceWorld(editorView ? ActivePreviewWorld() : nullptr);

            mWorld.Resource<InputState>().SetUiCapture(mEditor.WantsCaptureKeyboard(), mEditor.WantsCaptureMouse());
        #else
            mRenderSystem->SetViewportRect(0, VkRect2D{});
            mRenderSystem->SetViewportCamera(0, kNullEntity);
        #endif

            mScriptReloadWatcher.Poll(Assets(), dt);

            // advances in every mode; FixedUpdate systems check IsSimulating
            // themselves, as ScriptSystem does
            const uint32_t fixedSteps = mFixedTimestep.Advance(dt);

            SystemContext context = MakeContext(dt);
            if (!mDesc.mCapturePath.empty() && mFrame + 1 == mDesc.mMaxFrames)
                mRenderer.CaptureNextFrame(mDesc.mCapturePath);
            mScheduler.Update(context, fixedSteps, mFixedTimestep.mStep);

        #if MIR_WITH_EDITOR
            // after PrefabSystem committed their documents at this safe point
            if (IsEditing(mWorld) && mPrefabWorkspace != nullptr)
                mPrefabWorkspace->Update(dt, mElapsed, mFrame);
        #endif

            // reference clearing and binding changes at the safe point happen outside History
            if (PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>())
            {
                if (prefabs->ConsumePublishedChanges())
                    ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);
                if (prefabs->ConsumeSceneMaintenance() && !SceneUnsaved())
                    SetSceneUnsaved(true);
            }

            if (mWorld.Resource<InputState>().JustPressed(kToggleFullscreenAction))
                ToggleFullscreen();

            audio::Update();

            ++mFrame;

            RunSwitchHook();

            // same exit path as the close button
            if (mDesc.mMaxFrames != 0 && mFrame >= mDesc.mMaxFrames)
                mWindow->RequestClose();
        }
    }

    UIRect App::GameUIArea() const
    {
    #if MIR_WITH_EDITOR
        if (mDesc.mEnableEditorLayout)
            return ToUIRect(mEditor.SceneViewportRect());
    #endif
        return UIRect{ 0.0f, 0.0f, static_cast<float>(mWindow->Width()), static_cast<float>(mWindow->Height()) };
    }

#if MIR_WITH_EDITOR
    void App::ApplyEditorActions(const EditorActions &actions)
    {
        switch (actions.mScene)
        {
        case SceneMenuAction::New:
            NewScene();
            break;
        case SceneMenuAction::Save:
            SaveActiveDocument();
            break;
        case SceneMenuAction::Load:
            LoadScene();
            break;
        case SceneMenuAction::Play:
            PlayScene();
            break;
        case SceneMenuAction::TogglePause:
            TogglePause();
            break;
        case SceneMenuAction::Stop:
            StopScene();
            break;
        case SceneMenuAction::None:
            break;
        }

        bool assetActionAccepted = true;
        if (actions.mAssetImport.mKind == AssetImportActionKind::Remove)
        {
            AssetId target = MakeAssetId(actions.mAssetImport.mModelPath);
            for (const auto &model : mAssetImport.View().mModels)
                if (model.mPath == actions.mAssetImport.mModelPath && model.mAssetId != 0)
                    target = AssetId{ model.mAssetId };
            mWorld.GetOrCreateQuery<const ModelPlayer>().ForEach([&](Entity, const ModelPlayer &player)
                {
                    AssetId referenced = player.ModelAsset();
                    if (referenced.IsNull() && !player.ModelName().empty())
                        if (const AssetPathResult logical =
                            MakeLogicalAssetPath(mProject.mAssetIdRoot, player.ModelName());
                            logical.Ok())
                            referenced = MakeAssetId(logical.path);
                    assetActionAccepted &= referenced != target;
                });
            if (!assetActionAccepted)
                mAssetImport.Reject(actions.mAssetImport.mModelPath,
                    "cannot remove this asset while a scene entity references it");
        }
        if (assetActionAccepted)
            mAssetImport.Handle(actions.mAssetImport);

        if (actions.mContentBrowser.mKind == ContentBrowserActionKind::OpenInIde)
        {
            OpenInExternalIde(actions.mContentBrowser.mTargetPath);
        }
        else if (actions.mContentBrowser.mKind == ContentBrowserActionKind::OpenPrefab)
        {
            if (IsEditing(mWorld))
                OpenPrefabAsset(actions.mContentBrowser.mTargetPath);
        }
        else
        {
            bool contentBrowserActionAccepted = true;
            if (actions.mContentBrowser.mKind == ContentBrowserActionKind::Delete)
            {
                if (const std::optional<std::string> logical =
                    mContentBrowser.AssetLogicalPath(actions.mContentBrowser.mTargetPath);
                    logical.has_value())
                {
                    const AssetId target = MakeAssetId(*logical);
                    mWorld.GetOrCreateQuery<const ModelPlayer>().ForEach([&](Entity, const ModelPlayer &player)
                        {
                            AssetId referenced = player.ModelAsset();
                            if (referenced.IsNull() && !player.ModelName().empty())
                                if (const AssetPathResult logicalPlayer =
                                    MakeLogicalAssetPath(mProject.mAssetIdRoot, player.ModelName());
                                    logicalPlayer.Ok())
                                    referenced = MakeAssetId(logicalPlayer.path);
                            contentBrowserActionAccepted &= referenced != target;
                        });
                    if (!contentBrowserActionAccepted)
                        mContentBrowser.Reject(actions.mContentBrowser.mTargetPath,
                            "cannot delete this asset while a scene entity references it");
                }
            }
            if (contentBrowserActionAccepted)
                mContentBrowser.Handle(actions.mContentBrowser);
        }

        if (!actions.mOpenProject.empty())
            RequestOpenProject(actions.mOpenProject);

        if (!actions.mCreateProjectName.empty() &&
            CreateGameProject(actions.mCreateProjectName, actions.mCreateProjectRayTracing))
        {
            RequestOpenProject("games/" + actions.mCreateProjectName);
        }

        if (mPrefabWorkspace != nullptr && IsEditing(mWorld))
        {
            const DocumentId document = actions.mDocument.mDocument;
            const PrefabId prefab{ document.mHigh, document.mLow };
            switch (actions.mDocument.mKind)
            {
            case EditorDocumentActionKind::Activate:
                if (document == kSceneDocument)
                    mPrefabWorkspace->ActivateScene();
                else
                    mPrefabWorkspace->Open(prefab);
                break;
            case EditorDocumentActionKind::Close:
                if (document != kSceneDocument)
                    mPrefabWorkspace->Close(prefab);
                break;
            case EditorDocumentActionKind::None:
                break;
            }
        }

        if (actions.mSaveActiveDocument)
            SaveActiveDocument();
    }

    void App::OpenPrefabAsset(const std::string &relativePath)
    {
        if (mPrefabWorkspace == nullptr)
        {
            MIR_LOG_ERROR("Content browser: no Prefab documents are available for '{}'", relativePath);
            return;
        }

        const std::optional<PrefabId> id = mPrefabWorkspace->FindByPath(mProject.mRootDir / relativePath);
        if (!id.has_value())
        {
            MIR_LOG_ERROR("Content browser: '{}' is not a registered Prefab", relativePath);
            mContentBrowser.Reject(relativePath, "not a registered Prefab");
            return;
        }

        if (!mPrefabWorkspace->Open(*id))
        {
            MIR_LOG_ERROR("Content browser: could not open '{}'", relativePath);
            mContentBrowser.Reject(relativePath, "could not open this Prefab");
        }
    }

    World *App::ActivePreviewWorld()
    {
        if (IsEditing(mWorld) && mPrefabWorkspace != nullptr)
        {
            if (const std::optional<PrefabId> active = mPrefabWorkspace->ActiveDocument())
                return mPrefabWorkspace->PreviewWorld(*active);
        }
        return nullptr;
    }

    EditorDocuments App::MakeEditorDocuments()
    {
        EditorDocuments documents;
        documents.mTabs.push_back(
            EditorDocumentTab{ kSceneDocument, mScene.mName, mEditor.IsSceneUnsaved(), false });
        documents.mActive = kSceneDocument;
        documents.mEnabled = IsEditing(mWorld);

        if (mPrefabWorkspace != nullptr)
        {
            const PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
            for (const PrefabId id : mPrefabWorkspace->OpenDocuments())
            {
                const DocumentId document{ id.mHigh, id.mLow };
                const bool unsaved = prefabs == nullptr || !prefabs->Registry().HasSavedFile(id) ||
                    mEditor.IsDocumentUnsaved(document);
                documents.mTabs.push_back(
                    EditorDocumentTab{ document, mPrefabWorkspace->LabelOf(id), unsaved, true });
            }

            if (const std::optional<PrefabId> active = mPrefabWorkspace->ActiveDocument())
                documents.mActive = DocumentId{ active->mHigh, active->mLow };
        }
        return documents;
    }

    ContentBrowserView App::MakeContentBrowserView()
    {
        ContentBrowserView view = mContentBrowser.View();
        const PrefabCoordinator *prefabs = mWorld.TryResource<PrefabCoordinator>();
        if (prefabs == nullptr)
            return view;

        std::set<std::string> known;
        for (const ContentBrowserEntryView &entry : view.mEntries)
            known.insert(entry.mRelativePath);

        for (const PrefabId id : prefabs->DocumentIds())
        {
            if (!prefabs->Registry().HasMemoryDocument(id) || prefabs->Registry().HasSavedFile(id))
                continue;
            const std::optional<std::filesystem::path> intended = prefabs->Registry().IntendedPathOf(id);
            if (!intended)
                continue;

            const std::string relative = intended->lexically_relative(mProject.mRootDir).generic_string();
            if (relative.empty() || relative.rfind("..", 0) == 0)
                continue;

            for (std::size_t slash = relative.find('/'); slash != std::string::npos;
                 slash = relative.find('/', slash + 1))
            {
                const std::string directory = relative.substr(0, slash);
                if (!known.insert(directory).second)
                    continue;
                ContentBrowserEntryView entry;
                entry.mRelativePath = directory;
                entry.mName = std::filesystem::path(directory).filename().string();
                entry.mKind = ContentBrowserEntryKind::Directory;
                view.mEntries.push_back(std::move(entry));
            }

            if (!known.insert(relative).second)
                continue;
            ContentBrowserEntryView entry;
            entry.mRelativePath = relative;
            entry.mName = intended->filename().string();
            entry.mKind = ContentBrowserEntryKind::File;
            entry.mUnsaved = true;
            view.mEntries.push_back(std::move(entry));
        }
        return view;
    }

    void App::ApplyImportedAssets()
    {
        auto candidateManifest = AssetManifest::LoadFile(CookedAssetsDir() / kAssetManifestFileName);
        if (!candidateManifest)
        {
            mAssetImport.MarkApplyFailed("the new asset manifest could not be loaded");
            return;
        }
        AssetCache candidate(&*candidateManifest, CookedAssetsDir());
        std::string error;
        if (mModelSystem && !mModelSystem->PrepareReload(candidate, error))
        {
            mAssetImport.MarkApplyFailed("new model resources were rejected: " + error);
            return;
        }

        if (mPrefabWorkspace && !mPrefabWorkspace->PrepareReload(candidate, error))
        {
            if (mModelSystem)
                mModelSystem->DiscardPreparedReload();
            mAssetImport.MarkApplyFailed("new preview model resources were rejected: " + error);
            return;
        }

        mAssetCache.reset();
        mAssetManifest = std::move(candidateManifest);
        mAssetCache.emplace(&*mAssetManifest, CookedAssetsDir());
        if (mModelSystem)
            mModelSystem->CommitReload(&*mAssetCache);
        if (mMeshResolveSystem)
            mMeshResolveSystem->CommitReload(&*mAssetCache);
        if (mPrefabWorkspace)
            mPrefabWorkspace->CommitReload(&*mAssetCache);

        // a script whose asset arrived with this cook resolves now; resolved ones are skipped
        ResolveSceneScripts(mWorld, mScriptHost, mScriptReloadWatcher, Assets(), mProject.mAssetIdRoot);
        mAssetImport.MarkApplied();
        MIR_LOG_INFO("Applied imported assets at an Edit frame boundary");
    }

    void App::OpenInExternalIde(const std::string &relativePath)
    {
        const IdeSettings settings = IdeSettings::LoadFile(mProject.mProjectSettingsPath).value_or(IdeSettings{});
        const std::filesystem::path ide =
            settings.path.empty() ? DetectDefaultIde().value_or(std::filesystem::path{})
            : std::filesystem::path(settings.path);
        if (ide.empty())
        {
            MIR_LOG_WARN("Content browser: no IDE configured or detected for '{}'", relativePath);
            mContentBrowser.Reject(
                relativePath, "no IDE configured and none could be auto-detected - set one in Project Settings");
            return;
        }

        const std::filesystem::path absolute = mProject.mRootDir / relativePath;
        const std::vector<std::string> args = settings.BuildArgs(mProject.mRootDir, absolute);

        std::string argsLogged;
        for (const std::string &arg : args)
            argsLogged += (argsLogged.empty() ? "" : " ") + arg;
        MIR_LOG_INFO("Content browser: opening '{}' (project root '{}') with '{}' {}", absolute.string(),
            mProject.mRootDir.string(), ide.string(), argsLogged);

        if (!LaunchProcess(ide, args, false).has_value())
        {
            MIR_LOG_ERROR("Content browser: could not launch '{}' for '{}'", ide.string(), relativePath);
            mContentBrowser.Reject(relativePath, "could not launch '" + ide.string() + "'");
        }
    }
#endif

    // Runs in full after a partial Initialize too: every step below is a no-op on what was never built
    void App::Shutdown()
    {
        // let all the systems stop first
        SystemContext stopContext = MakeContext(0.0f);
        mScheduler.Stop(stopContext);

    #if MIR_WITH_EDITOR
        mAssetImport.Shutdown();
    #endif

        audio::Terminate();

        mScheduler.Reset();

        // Cache before manifest
        mAssetCache.reset();
        mAssetManifest.reset();

        DiscardSnapshot(mPlaySnapshot);

    #if MIR_WITH_EDITOR
        mEditor.Shutdown();
        mPrefabWorkspace.reset();
        mPrefabAuthoring.reset();
    #endif
        mImGui.Shutdown(mRenderer);

        mRenderer.Shutdown();
        mWindow.reset();

        // the systems Reset destroyed
        mRenderSystem = nullptr;
        mScriptSystem = nullptr;
        mModelSystem = nullptr;
        mMeshResolveSystem = nullptr;

        if (mInitialized)
            MIR_LOG_INFO("App shut down");
        mInitialized = false;
    }
} // namespace mir
