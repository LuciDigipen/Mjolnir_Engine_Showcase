/**
 * @file RendererModule.cpp
 * @author sumin.park
 * @brief The Vulkan renderer, renderer components and the systems that feed it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RendererModule.h>

#include <renderer/ActiveCamera.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/MeshResolveSystem.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <renderer/ViewDrawSource.h>
#include <renderer/components/MeshRenderer.h>

#include <assets/AssetsModule.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <core/log/SetupError.h>

namespace mir
{
    RendererModule::RendererModule(ModuleContext context, const ISurfaceProvider &surface, AssetsModule &assets,
        const RendererModuleSettings &settings)
    {
        if (!mRenderer.Initialize(
            { .window = &surface, .appName = settings.mAppName, .enableValidation = settings.mEnableValidation }))
        {
            // the destructor does not run for a throwing constructor
            mRenderer.Shutdown();
            throw SetupError(ErrorCategory::System, "The Vulkan renderer could not start", {},
                "the log above names the step that failed and its VkResult",
                "Update the GPU driver, and check that the GPU supports Vulkan.");
        }

        RegisterRendererComponents();
        RegisterRendererEntityTypes();
        RegisterRendererPostEffects();

        context.world.EmplaceResource<ViewDrawSources>();
        context.world.EmplaceResource<RenderSettings>(
            RenderSettings::LoadFile(settings.mProjectSettingsPath).value_or(RenderSettings{}));
        context.world.EmplaceResource<PostProcessingSettings>(
            PostProcessingSettings::LoadFile(settings.mProjectSettingsPath).value_or(PostProcessingSettings{}));

        // a WorldTransform reader
        mMeshes = &context.scheduler.AddSystem<MeshResolveSystem>(SystemPhase::LateUpdate,
            SystemOrder{SystemOrder::kNormal}, mRenderer, &assets.Cache(),
            settings.mAssetIdRoot);
        mActiveCamera = &context.scheduler.AddSystem<ActiveCameraSystem>(SystemPhase::Render,
            SystemOrder{SystemOrder::kEarly});
        mRenderSystem = &context.scheduler.AddSystem<RenderSystem>(SystemPhase::Render,
            SystemOrder{SystemOrder::kNormal}, mRenderer);

        mHooks = context.world.SetComponentHooks<MeshRenderer>({ .onRemove = [this](World &, Entity entity)
            {
                mMeshes->Forget(entity);
            } });

        // no scene entity holds a mesh or material handle now
        mSceneUnload = context.scheduler.AddStage(Stage::SceneUnload, [this](SystemContext &)
            {
                mMeshes->Reset();
                mRenderer.ReleaseSceneResources();
            });

        // a cut: the history of the scene before says nothing about this one
        mSceneLoad = context.scheduler.AddStage(Stage::SceneLoad, [this](SystemContext &)
            {
                mRenderer.ResetHistory();
                mActiveCamera->Reset();
                mRenderSystem->ResetCameraReports();
            });
        mPlayBegin = context.scheduler.AddStage(Stage::PlayBegin, [this](SystemContext &)
            {
                mRenderer.ResetHistory();
                mActiveCamera->Reset();
                mRenderSystem->ResetCameraReports();
            });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd, [this](SystemContext &) { mActiveCamera->Reset(); });

        mReloadCommit = context.scheduler.AddStage(Stage::ReloadCommit, [this, &assets](SystemContext &)
            {
                mMeshes->CommitReload(&assets.Cache());
            });
    }

    // the tokens first, so nothing they reach runs on a renderer already shut down
    RendererModule::~RendererModule()
    {
        mReloadCommit.Reset();
        mPlayEnd.Reset();
        mPlayBegin.Reset();
        mSceneLoad.Reset();
        mSceneUnload.Reset();
        mHooks.Reset();
        mRenderer.Shutdown();
    }
}
