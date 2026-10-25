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

#include <stdexcept>
#include <string>
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

        mHooks = context.world.SetComponentHooks<ModelPlayer>({ .onRemove = [this](World &world, Entity entity)
            {
                mModels->Forget(world, entity);
            } });

        mSceneUnload = context.scheduler.AddStage(Stage::SceneUnload, [this](SystemContext &frame)
            {
                mModels->Reset();
                frame.world.Resource<ModelDrawItems>().items.clear();
                frame.world.Resource<ModelMorphOverrides>().instances.clear();
            });

        // stages the new cook's GPU resources; a rejection vetoes the swap
        mReloadPrepare = context.scheduler.AddStage(Stage::ReloadPrepare, [this, &assets](SystemContext &)
            {
                std::string error;
                if (!mModels->PrepareReload(assets.Candidate(), error))
                    throw std::runtime_error("new model resources were rejected: " + error);
            });
        mReloadCommit = context.scheduler.AddStage(Stage::ReloadCommit, [this, &assets](SystemContext &)
            {
                mModels->CommitReload(&assets.Cache());
            });
        mReloadAbort = context.scheduler.AddStage(Stage::ReloadAbort, [this](SystemContext &)
            {
                mModels->DiscardPreparedReload();
            });
    }
}
