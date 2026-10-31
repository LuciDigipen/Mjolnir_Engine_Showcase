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
#include <string>

namespace mir
{
    class ActiveCameraSystem;
    class AssetsModule;
    class MeshResolveSystem;
    class RenderSystem;

    struct RendererModuleSettings
    {
        // Vulkan instance name
        const char *mAppName = "MjolnirEngine";
        bool mEnableValidation = true;
        // RenderSettings and PostProcessingSettings are read from it
        std::filesystem::path mProjectSettingsPath;
        std::string mAssetIdRoot;
        // Empty disables GPU timestamp capture.
        std::filesystem::path mGpuTimingPath;
    };

    class RendererModule
    {
    public:
        // throws SetupError (System) when Vulkan cannot start
        // surface is the window; the renderer never links window
        RendererModule(ModuleContext context, const ISurfaceProvider &surface, AssetsModule &assets,
            const RendererModuleSettings &settings);
        ~RendererModule();

        RendererModule(const RendererModule &) = delete;
        RendererModule &operator=(const RendererModule &) = delete;

        VulkanRenderer &Renderer() { return mRenderer; }
        RenderSystem &Rendering() { return *mRenderSystem; }
        MeshResolveSystem &Meshes() { return *mMeshes; }
        SurfaceMaterialLibrary &SurfaceMaterials() { return mSurfaceMaterials; }

    private:
        VulkanRenderer mRenderer;
        SurfaceMaterialLibrary mSurfaceMaterials;

        // owned by the scheduler, which outlives this module
        MeshResolveSystem *mMeshes = nullptr;
        ActiveCameraSystem *mActiveCamera = nullptr;
        RenderSystem *mRenderSystem = nullptr;

        // last: they unregister before the renderer above shuts down
        Registration mHooks;
        Registration mSceneUnload;
        Registration mSceneLoad;
        Registration mPlayBegin;
        Registration mPlayEnd;
        Registration mReloadPrepare;
        Registration mReloadAbort;
        Registration mReloadCommit;
    };
}
