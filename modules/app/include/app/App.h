/**
 * @file App.h
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <app/ProjectResources.h>
#include <project/GameProject.h>
#include <animation/AnimationModule.h>
#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <assets/AsyncAssetService.h>
#ifndef MIR_WITH_AUDIO
#error "MIR_WITH_AUDIO is set by linking mir::app"
#endif
#if MIR_WITH_AUDIO
#include <app/GameAudioOptions.h>
#include <audio/AudioModule.h>
#include <audio_script/AudioScriptModule.h>
#endif
#include <app/GameControlsOptions.h>
#include <camera/CameraModule.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/CoreModule.h>
#include <core/ecs/FixedTimestep.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <gameshell/GameShellModule.h>
#include <gameui/GameUIModule.h>
#include <gameui_imgui/GameUIImGuiModule.h>
#include <imguiruntime/ImGuiModule.h>
#include <input/InputModule.h>
#include <input_script/InputScriptModule.h>
#include <physics/PhysicsModule.h>
#include <physics_script/PhysicsScriptModule.h>
#include <particles/ParticlesModule.h>
#include <particles_script/ParticlesScriptModule.h>
#include <particles_runtime/ParticlesRuntimeModule.h>
#include <particles_gpu/ParticlesGpuModule.h>
#include <renderer/RendererModule.h>
#include <renderer_script/RendererScriptModule.h>
#include <prefab/PrefabModule.h>
#include <scene/SceneIO.h>
#include <scene/SceneLoader.h>
#include <scene/SceneModule.h>
#include <script/ScriptModule.h>
#include <terrain/TerrainModule.h>
#include <terrain_runtime/TerrainRuntimeModule.h>
#include <terrain_script/TerrainScriptModule.h>
#include <core/StartupGate.h>
#include <window/Splash.h>

#include <chrono>
#include <window/WindowModule.h>
#include <jobs/JobExecutor.h>

// 1 with the editor, 0 for the game alone
#ifndef MIR_WITH_EDITOR 
#error "MIR_WITH_EDITOR is set by linking mir::app"
#endif

#if MIR_WITH_EDITOR
#include <editor/EditorModule.h>
#include <terrain_editor/TerrainEditorModule.h>
#endif
#if MIR_WITH_AUDIO && MIR_WITH_EDITOR
#include <app/EditorAudioOptions.h>
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    struct AdditiveSceneRequest;

    struct AppDesc
    {
        uint32_t mWidth = 1280;
        uint32_t mHeight = 720;

        // Vulkan instance name. Engine-level, not project data
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;

        bool mEnableEditorLayout = true;

        float mMaxDeltaSeconds = 0.25f;

        // FixedUpdate: seconds per substep, and at most this many substeps a frame
        float mFixedDeltaSeconds = 1.0f / 60.0f;
        uint32_t mMaxFixedSteps = 4;

        // 0: run until the window closes
        uint64_t mMaxFrames = 0;
        std::filesystem::path mCapturePath;
        // Bounded automatic pool; --workers 0 explicitly selects serial execution.
        uint32_t mWorkerThreads = static_cast<uint32_t>(JobExecutor::DefaultWorkerCount());

        // debug: draw UIDemoScreen, every core widget, as the game UI
        bool mUIDemo = false;

        // for automated cases
        bool mNonInteractive = false;
        bool mHidden = false;

        // the game Initialize opens
        GameProject mProject;

        // test hook: RequestOpenProject this halfway through mMaxFrames; empty for
        // none
        std::string mSwitchProjectTo;
    };

    class App
    {
    public:
        //whether this build has the editor in it at all
        static constexpr bool kHasEditor = MIR_WITH_EDITOR != 0;

        // Builds every module in member order. A throw unwinds what was built, in reverse
        explicit App(const AppDesc &desc);
        ~App();

        App(const App &) = delete;
        App &operator=(const App &) = delete;
        App(App &&) = delete;
        App &operator=(App &&) = delete;

        void Run();

        World &GetWorld() { return mWorldState.world; }
        World *RuntimeWorld();
        SystemScheduler &Systems() { return mWorldState.scheduler; }

        // Temporary seam
        VulkanRenderer &Renderer() { return mRenderer.Renderer(); }

        // Startup splash
        SplashScreen &Splash() { return mSplash.screen; }

        // Never null; empty until a build or import cooks something
        AssetCache *Assets() { return &mAssets.Cache(); }

        // The background asset loader, one for the whole session even when the first cook comes later
        AsyncAssetService *AssetService() { return mAssetService.get(); }

        // The scene currently in the World
        LoadedScene &Scene() { return mWorldState.scene.Scene(); }

        // The game currently open
        const GameProject &Project() const { return mProject; }


        // Validates the game and closes the window; main.cpp then relaunches into it.
        // False (logged) if it is not a game.
        bool RequestOpenProject(const std::string &requestedDir);

        // The game RequestOpenProject accepted, if any
        const std::optional<GameProject> &ProjectToOpen() const { return mProjectToOpen; }

        // Discards the scene's entities and replaces it with an empty one. incomplete: it
        // stands in for a scene that could not be read, and stays untitled until saved
        void NewScene(std::string name = "untitled", bool incomplete = false);

        // Writes Scene() to the game's scene directory, asking first for an untitled scene.
        // False on I/O failure or when the user declines
        bool SaveScene();

        // Replaces Scene() with the game's saved scene and resolves it. False without a
        // scene.json, or when the user keeps the current scene over a broken one
        bool LoadScene();

        // Edit -> Play
        void PlayScene();

        // Play -> Edit
        void StopScene();

        // Toggles Running/Paused. No-op outside Play.
        void TogglePause();

        //windowed to fullscreen without a restart. called by f11.
        //swapchain is recreated next frame.
        void ToggleFullscreen();

    private:
        // shown while App is built and the first view loads, closed by RevealWindow or by its destructor
        struct StartupSplash
        {
            SplashScreen screen;
            explicit StartupSplash(const AppDesc &desc);
        };

        // a splash status line between two module members
        struct SplashStep
        {
            SplashStep(StartupSplash &splash, const char *status, float progress)
            {
                splash.screen.SetProgress(status, progress);
            }
        };

        ModuleContext Context();

        // context is rebuilt per tick
        SystemContext MakeContext(float dt);
        SystemContext MakeRuntimeContext(float dt);

        // what the editor, the game shell and the import asked for last frame
        void ApplyRequests();

        // Queues background preparation, then activates a current result at the frame boundary.
        bool SwapScene(const std::string &id);
        void PollSceneLoad();
        bool ActivateScene(const SceneLoadCompletion &completed);
        bool LoadAdditiveScene(const AdditiveSceneRequest &request);
        bool UnloadRuntimeScene(SceneInstanceId instance, bool partial = true);
        bool SetPrimaryScene(SceneInstanceId instance);
        void RefreshPrimaryScene();

        // PlayEnd, every scene's unload and the runtime systems' Stop, in the order Stop runs them.
        void StopRuntimeStages();

        // A standalone game starts no gameplay for a scene until its resources are ready. The window, the
        // completions, the uploads and the loading UI keep running, and only the resource systems tick. False
        // when the window closed, or a bounded run ran out of frames, before the scene was ready.
        bool AwaitSceneResources();
        void DrawLoadingOverlay();

        // True when the editor has material drafts that are not accepted yet.
        bool HasUnsavedMaterialDrafts() const;

        // One guard for OS close, Alt+F4, the custom title bar and Request::Quit.
        bool ConfirmEditorExit();

        // destroys the scene's entities, then the SceneUnload stage frees what they held
        void UnloadCurrentScene();

        // the functions modules registered for it, then a command flush
        void RunStage(Stage stage);

        // LoadScene if the game has a scene.json; throws SetupError for a broken one the user won't open
        void LoadProjectScene();

        enum class SceneLoadChoice
        {
            Load,
            StartEmpty,
            Refuse,
        };

        // Asks what to do with a scene ProbeScene found broken; Refuse when nobody can answer
        SceneLoadChoice ChooseSceneLoad(const SceneProbe &probe, const std::filesystem::path &directory, bool atStartup) const;

        // Asks whether an untitled scene may replace the broken one in the scene directory
        bool ConfirmBindUntitledScene() const;

        // Unload and load, with no checks; incomplete when entity files are missing
    #if MIR_WITH_EDITOR
        void CreateScene(const std::string &id, const std::string &path, bool saveCurrent);
    #endif
        bool OpenScene(const std::string &id, bool saveCurrent);

        void ReplaceSceneFromDisk(bool incomplete);

        // a frame's check of the first view's loading, until the window opens
        void WaitForFirstView();

        // resumes the prepared runtime, shows the window and closes the splash
        void RevealWindow();

        // the mSwitchProjectTo test hook
        void RunSwitchHook();

    #if MIR_WITH_EDITOR
        // swaps in what a finished import cooked, at an Edit frame boundary
        void ApplyImportedAssets();
    #endif

        // Startup order, which is dependency order: a module after every module it uses,
        // a bridge after script and its library. C++ destroys them in reverse. Systems with
        // equal orders in a phase run in this order too, but nothing may rely on it
        AppDesc mDesc;
        GameProject mProject;
        std::string mOpenSceneId = mProject.mStartupSceneId;
        StartupSplash mSplash;
        JobExecutor mJobs;
        SceneLoader mSceneLoader{ mJobs };

        struct WorldState
        {
            WorldState(ProjectResources &resources, const GameProject &project, AssetsModule &assets,
                const AssetResolver &resolver, GameShellSettings shellSettings);

            // Shares the project resources first, so every module member below finds them in the World.
            struct ProjectShare
            {
                ProjectShare(World &world, ProjectResources &resources) { resources.ShareInto(world); }
            };

            CommandBuffer commands;
            World world;
            SystemScheduler scheduler;
            CoreModule core{ ModuleContext{ world, scheduler }, commands };
            ProjectShare projectShare;

            ParticlesModule particles;
            SceneModule scene;
            PrefabModule prefab;
            GameUIModule gameUI;
            GameShellModule gameShell;

            ModuleContext Context() { return { world, scheduler }; }
        };

        AssetsModule mAssets;
        std::unique_ptr<AsyncAssetService> mAssetService;
        AssetResolver mResolver;
        // every World borrows from it, so it outlives them
        ProjectResources mProjectResources{ mProject };
        ControlsSettings mControls;
        GameControlsOptions mControlsOptions;
        // before ImGuiModule: ImGui's shutdown frees the GPU side of the UI's textures,
        // so the textures must still be there
        WorldState mWorldState;
        WindowModule mWindow;
        SplashStep mRendererStep{ mSplash, "Initializing renderer...", 0.2f };
        RendererModule mRenderer;
        ParticlesGpuModule mParticlesGpu;
        // before ImGui and the editor, which read input and models
        InputModule mInput;
        AnimationModule mAnimation;
        SplashStep mImGuiStep{ mSplash, "Initializing UI...", 0.4f };
        ImGuiModule mImGui;
    #if MIR_WITH_EDITOR
        SplashStep mEditorStep{ mSplash, "Initializing editor...", 0.5f };
        EditorModule mEditor;
        TerrainEditorModule mTerrainEditor;
    #endif
        SplashStep mModulesStep{ mSplash, "Registering components...", 0.75f };
        CameraModule mCamera;
    #if MIR_WITH_AUDIO
        AudioModule mAudio;
        // handed to mWorldState.gameShell, built before it, once both are up
        GameAudioOptions mAudioOptions{ mAudio };
    #if MIR_WITH_EDITOR
        // handed to mEditor the same way
        EditorAudioOptions mEditorAudio{ mAudio, mProject.mSoundBankDir };
    #endif
    #endif
        struct SimulationSystems
        {
            SimulationSystems(ModuleContext context, const GameProject &project, JobExecutor &jobs,
                AssetsModule &assets, const AssetResolver &resolver, ParticlesModule &particles,
                ParticlesGpuModule &particlesGpu, RenderSystem &rendering);

            PhysicsModule physics;
            TerrainModule terrainData;
            ScriptModule script;
            ParticlesRuntimeModule particlesRuntime;
            ParticlesScriptModule particlesScript;
        #if MIR_WITH_AUDIO
            AudioScriptModule audioScript;
        #endif
            InputScriptModule inputScript;
            PhysicsScriptModule physicsScript;
            RendererScriptModule rendererScript;
            TerrainScriptModule terrainScript;
        };

        SimulationSystems mSimulation;
        TerrainRuntimeModule mTerrain;
        GameUIImGuiModule mGameUIImGui;

        struct RuntimeSession;
        std::unique_ptr<RuntimeSession> mRuntime;

        // set until the window opens; a bounded or hidden run does not wait
        std::optional<StartupGate> mFirstView;
        std::chrono::steady_clock::time_point mFirstViewStart;
        std::optional<GameProject> mProjectToOpen;
        bool mSwitchHookFired = false;
        double mElapsed = 0.0;
        uint64_t mFrame = 0;
        FixedTimestep mFixedTimestep;

        bool mGateScenes = false;
        bool mSceneGated = false;
        // last: it unregisters before anything it draws from is destroyed
        Registration mLoadingDrawer;
    };
} // namespace mir
