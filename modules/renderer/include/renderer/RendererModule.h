/**
 * @file RendererModule.h
 * @author sumin.park
 * @brief The Vulkan renderer, renderer components and the systems that feed it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/VulkanRenderer.h>

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <filesystem>
#include <memory>
#include <string>

namespace mir
{
    class ActiveCameraSystem;
    class AssetResolver;
    class AsyncAssetService;
    class MeshResolveSystem;
    class RenderSystem;

    struct RendererModuleSettings
    {
        // Vulkan instance name
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;
        // looks up the names a MeshRenderer holds, required
        const AssetResolver *mResolver = nullptr;
        // loads mesh assets in the background, null disables mesh asset loading
        AsyncAssetService *mAssetService = nullptr;
        bool mAdvanceSharedFrame = true;
    };

    class RendererModule
    {
    public:
        // throws SetupError (System) when Vulkan cannot start
        // surface is the window; the renderer never links window
        RendererModule(ModuleContext context, const ISurfaceProvider &surface,
            const RendererModuleSettings &settings);
        ~RendererModule();

        RendererModule(const RendererModule &) = delete;
        RendererModule &operator=(const RendererModule &) = delete;

        // Binds render resources and systems to an additional world using this device.
        class WorldBinding;
        std::unique_ptr<WorldBinding> BindWorld(ModuleContext context, const RendererModuleSettings &settings);
        void ActivateWorld(WorldBinding *binding);

        VulkanRenderer &Renderer() { return mRenderer; }
        RenderSystem &Rendering();
        MeshResolveSystem &Meshes();
        SurfaceMaterialLibrary &SurfaceMaterials() { return mSurfaceMaterials; }

    public:
        class WorldBinding
        {
            friend class RendererModule;
            WorldBinding(ModuleContext context, VulkanRenderer &renderer, SurfaceMaterialLibrary &surfaceMaterials,
                const RendererModuleSettings &settings);

        public:
            ~WorldBinding();
            WorldBinding(const WorldBinding &) = delete;
            WorldBinding &operator=(const WorldBinding &) = delete;
            RenderSystem &Rendering() { return *mRenderSystem; }
            MeshResolveSystem &Meshes() { return *mMeshes; }

        private:
            VulkanRenderer &mRenderer;
            SurfaceMaterialLibrary &mSurfaceMaterials;

            // owned by the scheduler, which outlives this module
            MeshResolveSystem *mMeshes = nullptr;
            ActiveCameraSystem *mActiveCamera = nullptr;
            RenderSystem *mRenderSystem = nullptr;

            // The shared budgets last applied to the renderer, by the main binding only.
            double mAppliedUploadMiB = 0.0;
            double mAppliedResidencyMiB = 0.0;

            // last: they unregister before the renderer above shuts down
            Registration mHooks;
            Registration mSceneUnload;
            Registration mSceneLoad;
            Registration mPlayBegin;
            Registration mPlayEnd;
            Registration mFrameBegin;
            Registration mReloadCommit;
        };

    private:
        VulkanRenderer mRenderer;
        SurfaceMaterialLibrary mSurfaceMaterials;
        std::unique_ptr<WorldBinding> mWorldBinding;
        WorldBinding *mActiveWorldBinding = nullptr;
    };
}
