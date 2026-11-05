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
#include <renderer/RenderSettings.h>
#include <renderer/RenderSystem.h>
#include <renderer/ViewDrawSource.h>
#include <renderer/components/MeshRenderer.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <core/log/Assert.h>
#include <core/log/SetupError.h>

#include <stdexcept>

namespace mir
{
    RendererModule::RendererModule(ModuleContext context, const ISurfaceProvider &surface,
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
        mSurfaceMaterials.Bind(settings.mAssetService, mRenderer);
        // A committed shader generation may have migrated every surface
        // record, so the library's cached metadata refreshes without a panel.
        mRenderer.SetShaderReloadListener([this] { mSurfaceMaterials.NotifyShaderCommit(); });
        mRenderer.SetMaterialRecordValidator([this](std::string_view frontend, const MaterialLayout &layout,
            std::span<const std::byte> bytes, std::string &error)
            {
                return mSurfaceMaterials.ValidateRecord(frontend, layout, bytes, error);
            });

        mWorldBinding = BindWorld(context, settings);
        mActiveWorldBinding = mWorldBinding.get();
    }

    void RendererModule::ActivateWorld(WorldBinding *binding)
    {
        mActiveWorldBinding = binding != nullptr ? binding : mWorldBinding.get();
        mRenderer.ResetHistory();
    }

    RenderSystem &RendererModule::Rendering() { return mActiveWorldBinding->Rendering(); }

    MeshResolveSystem &RendererModule::Meshes() { return mActiveWorldBinding->Meshes(); }

    std::unique_ptr<RendererModule::WorldBinding> RendererModule::BindWorld(ModuleContext context,
        const RendererModuleSettings &settings)
    {
        return std::unique_ptr<WorldBinding>(new WorldBinding(context, mRenderer, mSurfaceMaterials, settings));
    }

    RendererModule::WorldBinding::WorldBinding(ModuleContext context, VulkanRenderer &renderer,
        SurfaceMaterialLibrary &surfaceMaterials, const RendererModuleSettings &settings)
        : mRenderer(renderer), mSurfaceMaterials(surfaceMaterials)
    {
        context.world.EmplaceResource<ViewDrawSources>();
        // physics fills it while a view shows colliders
        context.world.EmplaceResource<DebugLines>();
        // The renderer is shared, so only the main binding applies the budgets.
        if (settings.mAdvanceSharedFrame)
        {
            const RenderSettings &renderSettings = context.world.Resource<RenderSettings>();
            mRenderer.SetUploadFrameBudget(renderSettings.UploadFrameBytes());
            mRenderer.SharedResidency()->SetTotalTarget(renderSettings.ResidencyBytes());
        }

        MIR_ASSERT(settings.mResolver != nullptr, "RendererModule: settings.mResolver is required");

        // a WorldTransform reader
        mMeshes = &context.scheduler.AddSystem<MeshResolveSystem>(SystemPhase::LateUpdate,
            SystemOrder{ SystemOrder::kNormal }, mRenderer, settings.mAssetService,
            *settings.mResolver, &mSurfaceMaterials);
        mActiveCamera = &context.scheduler.AddSystem<ActiveCameraSystem>(SystemPhase::Render,
            SystemOrder{ SystemOrder::kEarly });
        mRenderSystem = &context.scheduler.AddSystem<RenderSystem>(SystemPhase::Render,
            SystemOrder{ SystemOrder::kNormal }, mRenderer);

        mHooks = context.world.SetComponentHooks<MeshRenderer>({ .onRemove = [this](World &, Entity entity)
            {
                mMeshes->Forget(entity);
            } });

        // no scene entity holds a mesh or material handle now
        mSceneUnload = context.scheduler.AddStage(Stage::SceneUnload, [this](SystemContext &frame)
            {
                if (frame.sceneSwapped) return;
                mMeshes->ReleaseResources();
                mSurfaceMaterials.SetActiveMaterials({});
                // Independent Prefab Worlds keep their shared handles and preview values.
                if (mSurfaceMaterials.ActiveMaterials().empty())
                    mSurfaceMaterials.Reset();
                mRenderer.ResetHistory();
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

        if (settings.mAdvanceSharedFrame)
            mFrameBegin = context.scheduler.AddFrameSlot(FrameSlot::FrameBegin, [this](SystemContext &)
                {
                    mSurfaceMaterials.Advance();
                });

        mReloadCommit = context.scheduler.AddStage(Stage::ReloadCommit, [this](SystemContext &)
            {
                mMeshes->CommitReload();
                mSurfaceMaterials.CommitReload();
            });
    }

    RendererModule::WorldBinding::~WorldBinding()
    {
        mReloadCommit.Reset();
        mPlayEnd.Reset();
        mFrameBegin.Reset();
        mPlayBegin.Reset();
        mSceneLoad.Reset();
        mSceneUnload.Reset();
        mHooks.Reset();
        // The scheduler outlives this module, so the resolver must stop
        // reaching the library before the library object goes away.
        if (mMeshes != nullptr)
            mMeshes->DetachSurfaceMaterials();
    }

    RendererModule::~RendererModule()
    {
        mWorldBinding.reset();
        mRenderer.SetShaderReloadListener({});
        mRenderer.SetMaterialRecordValidator({});
        mRenderer.Shutdown();
    }
}
