/**
 * @file AnimationModule.h
 * @author sumin.park
 * @brief ModelPlayer, the model draw resources and ModelSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>

#include <string>

namespace mir
{
    class AsyncAssetService;
    class ModelSystem;
    class AssetResolver;
    class VulkanRenderer;
    class JobExecutor;

    class AnimationModule
    {
    public:
        AnimationModule(ModuleContext context, VulkanRenderer &renderer, AsyncAssetService *assets,
            const AssetResolver &resolver, JobExecutor *jobs = nullptr);

        AnimationModule(const AnimationModule &) = delete;
        AnimationModule &operator=(const AnimationModule &) = delete;

        ModelSystem &Models() { return *mModels; }

    private:
        // owned by the scheduler, which outlives this module
        ModelSystem *mModels = nullptr;

        // last: they unregister before anything above is destroyed
        Registration mHooks;
        Registration mSceneUnload;
        Registration mReloadCommit;
    };
}
