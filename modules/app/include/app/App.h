/**
 * @file App.h
 * @author Sumin Park
 * @brief Owns the engine subsystems and drives the main loop.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <project/GameProject.h>
#include <animation/AnimationModule.h>
#include <assets/AssetsModule.h>
#include <audio/AudioModule.h>
#include <audio_script/AudioScriptModule.h>
#include <camera/CameraModule.h>
#include <core/ecs/CommandBuffer.h>
#include <core/ecs/CoreModule.h>
#include <core/ecs/FixedTimestep.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <gameshell/GameShell.h>
#include <gameui/UIContext.h>
#include <gameui/UIDemoScreen.h>
#include <imguiruntime/ImGuiModule.h>
#include <input/InputModule.h>
#include <input_script/InputScriptModule.h>
#include <physics/PhysicsModule.h>
#include <physics_script/PhysicsScriptModule.h>
#include <renderer/RendererModule.h>
#include <renderer_script/RendererScriptModule.h>
#include <prefab/PrefabModule.h>
#include <scene/SceneIO.h>
#include <scene/SceneModule.h>
#include <script/ScriptModule.h>
#include <terrain/TerrainModule.h>
#include <terrain_runtime/TerrainRuntimeModule.h>
#include <terrain_script/TerrainScriptModule.h>
#include <window/Splash.h>
#include <window/WindowModule.h>

// 1 with the editor, 0 for the game alone
#ifndef MIR_WITH_EDITOR 
#error "MIR_WITH_EDITOR is set by linking mir::app"
#endif

#if MIR_WITH_EDITOR
#include <editor/EditorModule.h>
#endif

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{

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

        // debug: draw UIDemoScreen, every core widget, as the game UI
        bool mUIDemo = false;

        // for automated cases
        bool mNonInteractive = false;

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

        App();
        ~App();

        App(const App &) = delete;
        App &operator=(const App &) = delete;
        App(App &&) = delete;
        App &operator=(App &&) = delete;

        // Throws SetupError on any startup failure; the destructor then cleans up what was built
        void Initialize(const AppDesc &desc);
        void Run();

        // Safe on a partial Initialize and safe to call twice
        void Shutdown();

        World &GetWorld() { return mWorld; }
        SystemScheduler &Systems() { return mScheduler; }

        // Temporary seam
        VulkanRenderer &Renderer() { return mRenderer->Renderer(); }

        // Startup splash
        SplashScreen &Splash() { return mSplash; }

        // Never null after Initialize; empty until a build or import cooks something
        AssetCache *Assets();

        // The scene currently in the World
        LoadedScene &Scene() { return mSceneModule->Scene(); }

        // The game currently open
        const GameProject &Project() const { return mProject; }


        // Validates the game and closes the window; main.cpp then relaunches into it.
        // False (logged) if it is not a game.
        bool RequestOpenProject(const std::string &requestedDir);

        // The game RequestOpenProject accepted, if any
        const std::optional<GameProject> &ProjectToOpen() const { return mProjectToOpen; }

        // Discards the scene's entities and replaces it with an empty one. incomplete: it
        // stands in for a scene that could not be read
        void NewScene(std::string name = "untitled", bool incomplete = false);

        // Writes Scene() to the game's scene directory. False on I/O failure
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

        //queues what the player asked of the game in the pause menu
        void ApplyShellAction(GameShellAction action);

        //windowed to fullscreen without a restart. called by f11.
        //swapchain is recreated next frame.
        void ToggleFullscreen();

    private:
        ModuleContext Context();

        // the editor, the game UI and their actions; one ImGui drawer
        void DrawUI();

        // context is rebuilt per tick
        SystemContext MakeContext(float dt);

        // what the editor, the game shell and the import asked for last frame
        void ApplyRequests();

        // UnloadScene, then free the GPU resources
        void UnloadCurrentScene();

        //every handle the editor's undo history holds dies with the scene it came from
        void ClearHistory();

        //true while the scene has changes since its last save, shown as the title bar's *
        bool SceneUnsaved() const;

        //false = the scene counts as saved from now on, so the * disapears
        //true = the * stays until the next save
        void SetSceneUnsaved(bool unsaved);

        // LoadScene if the game has a scene.json; throws SetupError for a broken one the user won't open
        void LoadProjectScene();

        enum class SceneLoadChoice
        {
            Load,
            StartEmpty,
            Refuse,
        };

        // Asks what to do with a scene ProbeScene found broken; Refuse when nobody can answer
        SceneLoadChoice ChooseSceneLoad(const SceneProbe &probe, bool atStartup) const;

        // Unload and load, with no checks; incomplete when entity files are missing
        void ReplaceSceneFromDisk(bool incomplete);

        // the mSwitchProjectTo test hook
        void RunSwitchHook();

        //the game UI's area: the editor's scene viewport, or the whole window
        UIRect GameUIArea() const;

    #if MIR_WITH_EDITOR
        // swaps in what a finished import cooked, at an Edit frame boundary
        void ApplyImportedAssets();
    #endif

        SplashScreen mSplash;
        std::optional<WindowModule> mWindow;
        std::optional<RendererModule> mRenderer;
        std::optional<ImGuiModule> mImGui;
        // Stop puts the World back as it was at Play, so it puts this back too
        bool mUnsavedBeforePlay = false;
        UIContext mGameUI;
        UIDemoScreen mUIDemo;
        GameShell mGameShell;

            // Before mWorld, so it is destroyed after it
        CommandBuffer mCommands;

        World mWorld;
        SystemScheduler mScheduler;
        std::optional<CoreModule> mCore;
        std::optional<SceneModule> mSceneModule;
        std::optional<PrefabModule> mPrefab;
        std::optional<InputModule> mInput;
        std::optional<CameraModule> mCamera;
        std::optional<AnimationModule> mAnimation;
        std::optional<AudioModule> mAudio;
        std::optional<PhysicsModule> mPhysics;
        std::optional<TerrainModule> mTerrainData;
        std::optional<ScriptModule> mScript;
        std::optional<AudioScriptModule> mAudioScript;
        std::optional<InputScriptModule> mInputScript;
        std::optional<PhysicsScriptModule> mPhysicsScript;
        std::optional<RendererScriptModule> mRendererScript;
        std::optional<TerrainScriptModule> mTerrainScript;
        std::optional<TerrainRuntimeModule> mTerrain;
    #if MIR_WITH_EDITOR
        std::optional<EditorModule> mEditor;
    #endif



        AppDesc mDesc;
        GameProject mProject;
    #if MIR_WITH_EDITOR
    #endif
        std::optional<GameProject> mProjectToOpen;
        bool mSwitchHookFired = false;
        float mFrameDt = 0.0f;
        Registration mUiDrawer;
        double mElapsed = 0.0;
        uint64_t mFrame = 0;
        FixedTimestep mFixedTimestep;

        std::optional<AssetsModule> mAssets;
        bool mInitialized = false;
    };
} // namespace mir
