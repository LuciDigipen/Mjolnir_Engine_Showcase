/**
 * @file RendererModule.cpp
 * @author sumin.park
 * @brief The Vulkan renderer, renderer components and the systems that feed it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RendererModule.h>

#include <renderer/ComponentRegistration.h>
#include <renderer/MeshResolveSystem.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <renderer/ViewDrawSource.h>

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
        mMeshes = &context.scheduler.AddSystem<MeshResolveSystem>(SystemPhase::LateUpdate, mRenderer, &assets.Cache(),
            settings.mAssetIdRoot);
        mRenderSystem = &context.scheduler.AddSystem<RenderSystem>(SystemPhase::Render, mRenderer);
    }

    RendererModule::~RendererModule() { mRenderer.Shutdown(); }
}
