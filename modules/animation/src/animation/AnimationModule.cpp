/**
 * @file AnimationModule.cpp
 * @author sumin.park
 * @brief ModelPlayer, the model draw resources and ModelSystem.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <animation/AnimationModule.h>

#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>

#include <assets/AssetsModule.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <utility>

namespace mir
{
    AnimationModule::AnimationModule(ModuleContext context, VulkanRenderer &renderer, AssetsModule &assets,
        std::string assetIdRoot)
    {
        RegisterModelPlayer();

        context.world.EmplaceResource<ModelDrawItems>();
        context.world.EmplaceResource<ModelMorphOverrides>();

        // a WorldTransform reader
        mModels = &context.scheduler.AddSystem<ModelSystem>(SystemPhase::LateUpdate, renderer, &assets.Cache(),
            std::move(assetIdRoot));
    }
}
