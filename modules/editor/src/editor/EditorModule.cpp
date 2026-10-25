/**
 * @file EditorModule.cpp
 * @author sumin.park
 * @brief The editor: its UI drawer, scene ops, asset import and content browser.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/EditorModule.h>

#include <editor/CreateGameProject.h>
#include <editor/IdeSettings.h>

#include <animation/ModelPlayer.h>
#include <assets/AssetId.h>
#include <assets/AssetIdRoot.h>
#include <assets/AssetPath.h>
#include <assets/AssetsModule.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/Requests.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>
#include <core/platform/Process.h>
#include <imguiruntime/ImGuiModule.h>
#include <input/InputMap.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <renderer/RendererModule.h>
#include <scene/SceneModule.h>
#include <window/WindowModule.h>

#include <filesystem>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        // games found at startup, each with why it cannot open, if it cannot
        std::vector<ProjectMenuEntry> BuildProjectMenu()
        {
            std::vector<ProjectMenuEntry> menu;
            for (GameProject &project : DiscoverGameProjects())
            {
                // listed with the reason, not hidden: a game that vanished from the menu reads as lost
                ProjectMenuEntry entry{ project.mDir, project.mName, {} };
                try
                {
                    ResolveAssetIdRoot(project);
                }
                catch (const SetupError &error)
                {
                    entry.mUnavailable = error.What() + " (" + error.Cause() + ")";
                }
                menu.push_back(std::move(entry));
            }
            return menu;
        }

        // true while a scene entity plays the model
        bool IsModelReferenced(World &world, const std::string &assetIdRoot, AssetId model)
        {
            bool referenced = false;
            world.GetOrCreateQuery<const ModelPlayer>().ForEach([&](Entity, const ModelPlayer &player)
                {
                    AssetId played = player.ModelAsset();
                    if (played.IsNull() && !player.ModelName().empty())
                    {
                        const AssetPathResult logical = MakeLogicalAssetPath(assetIdRoot, player.ModelName());
                        if (logical.Ok())
                            played = MakeAssetId(logical.path);
                    }
                    referenced |= played == model;
                });
            return referenced;
        }
    }

    EditorModule::EditorModule(ModuleContext context, ImGuiModule &imgui, WindowModule &window,
        RendererModule &renderer, SceneModule &scene, AssetsModule &assets, const GameProject &project,
        bool enableLayout)
        : mWorld(context.world), mRenderer(renderer), mProject(project), mEnableLayout(enableLayout),
          mProjectMenu(BuildProjectMenu()), mSceneOps(context.world, scene.Scene())
    {
        mImport.Initialize(mProject);
        mContentBrowser.Initialize(mProject);

        // The first frames run without assets; the import swaps them in once cooked
        if (!assets.HadManifest() && mImport.SourceHasAssets())
            mImport.QueueFullCook();

        if (!mEditor.Initialize(imgui.Runtime(), window.Get(), mProject.mProjectSettingsPath, context.world, mSceneOps,
                renderer.Renderer().SwapchainIsSrgb()))
        {
            mEditor.Shutdown();
            mImport.Shutdown();
            throw SetupError(ErrorCategory::Internal, "The editor could not start", {},
                "the log above names the step that failed",
                "This is an engine bug. Report it with the log attached: " + LogFilePath().string());
        }

        mFrameBegin = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin, [this](SystemContext &frame)
            {
                mFrameDt = frame.dt;
                mImport.Update(IsEditing(mWorld));
                if (mImport.PendingApply() && IsEditing(mWorld))
                    mWorld.Resource<RequestQueue>().Push(Request::ApplyImportedAssets);
                mContentBrowser.Update();
            });

        mDrawer = imgui.AddDrawer([this, &window]
            {
                if (window.Get().Width() == 0 || window.Get().Height() == 0)
                    return;

                const EditorActions actions = mEditor.DrawLayout(mEnableLayout, mWorld.Resource<InputMap>(),
                    mWorld.Resource<RenderSettings>(), mWorld.Resource<PostProcessingSettings>(),
                    mWorld.Resource<EngineModeState>(), mProjectMenu, mProject.mDir, mWorld, mImport.View(),
                    mRenderer.Renderer(), mContentBrowser.View());
                ApplyActions(actions);
                FeedViewport();
            });
    }

    EditorModule::~EditorModule()
    {
        mDrawer.Reset();
        mFrameBegin.Reset();
        mEditor.Shutdown();
        // stops a running cooker
        mImport.Shutdown();
    }

    void EditorModule::FeedViewport()
    {
        RenderSystem &rendering = mRenderer.Rendering();
        rendering.SetViewportRect(0, mEnableLayout ? mEditor.SceneViewportRect() : VkRect2D{});

        // only Edit feeds the editor camera
        if (IsEditing(mWorld) && mEnableLayout)
        {
            mEditor.UpdateCamera(mFrameDt);
            const EditorCamera &camera = mEditor.SceneCamera();
            rendering.SetExternalCamera(0, camera.Pose(), camera.mLens);
        }
        else
        {
            rendering.SetViewportCamera(0, kNullEntity);
        }
    }

    void EditorModule::ApplyActions(const EditorActions &actions)
    {
        RequestQueue &requests = mWorld.Resource<RequestQueue>();
        switch (actions.mScene)
        {
        case SceneMenuAction::New:
            requests.Push(Request::NewScene);
            break;
        case SceneMenuAction::Save:
            requests.Push(Request::SaveScene);
            break;
        case SceneMenuAction::Load:
            requests.Push(Request::LoadScene);
            break;
        case SceneMenuAction::Play:
            requests.Push(Request::Play);
            break;
        case SceneMenuAction::TogglePause:
            requests.Push(IsPaused(mWorld) ? Request::Resume : Request::Pause);
            break;
        case SceneMenuAction::Stop:
            requests.Push(Request::Stop);
            break;
        case SceneMenuAction::None:
            break;
        }

        bool assetActionAccepted = true;
        if (actions.mAssetImport.mKind == AssetImportActionKind::Remove)
        {
            AssetId target = MakeAssetId(actions.mAssetImport.mModelPath);
            for (const auto &model : mImport.View().mModels)
                if (model.mPath == actions.mAssetImport.mModelPath && model.mAssetId != 0)
                    target = AssetId{ model.mAssetId };
            assetActionAccepted = !IsModelReferenced(mWorld, mProject.mAssetIdRoot, target);
            if (!assetActionAccepted)
                mImport.Reject(actions.mAssetImport.mModelPath,
                    "cannot remove this asset while a scene entity references it");
        }
        if (assetActionAccepted)
            mImport.Handle(actions.mAssetImport);

        if (actions.mContentBrowser.mKind == ContentBrowserActionKind::OpenInIde)
        {
            OpenInExternalIde(actions.mContentBrowser.mTargetPath);
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
                    contentBrowserActionAccepted = !IsModelReferenced(mWorld, mProject.mAssetIdRoot, MakeAssetId(*logical));
                    if (!contentBrowserActionAccepted)
                        mContentBrowser.Reject(actions.mContentBrowser.mTargetPath,
                            "cannot delete this asset while a scene entity references it");
                }
            }
            if (contentBrowserActionAccepted)
                mContentBrowser.Handle(actions.mContentBrowser);
        }

        if (!actions.mOpenProject.empty())
            requests.PushOpenProject(actions.mOpenProject);

        if (!actions.mCreateProjectName.empty() &&
            CreateGameProject(actions.mCreateProjectName, actions.mCreateProjectRayTracing))
        {
            requests.PushOpenProject("games/" + actions.mCreateProjectName);
        }
    }

    void EditorModule::OpenInExternalIde(const std::string &relativePath)
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
}
