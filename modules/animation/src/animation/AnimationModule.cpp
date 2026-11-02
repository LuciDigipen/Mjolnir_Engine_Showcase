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
#include <animation/ModelLibrary.h>
#include <animation/ModelSystem.h>

#include <assets/AsyncAssetService.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <stdexcept>
#include <string>
#include <utility>

namespace mir
{
    AnimationModule::AnimationModule(ModuleContext context, VulkanRenderer &renderer, AsyncAssetService *assets,
        const AssetResolver &resolver, JobExecutor *jobs)
    {
        RegisterModelPlayer();

        context.world.EmplaceResource<ModelDrawItems>();
        context.world.EmplaceResource<ModelMorphOverrides>();

        // a WorldTransform reader
        mLibrary = std::make_shared<ModelLibrary>(renderer, renderer, renderer.ErrorTexture());
        mModels = &context.scheduler.AddSystem<ModelSystem>(SystemPhase::LateUpdate,
            SystemOrder{SystemOrder::kNormal}, mLibrary, assets,
            resolver, jobs);

        mHooks = context.world.SetComponentHooks<ModelPlayer>({ .onRemove = [this](World &world, Entity entity)
            {
                mModels->Forget(world, entity);
            } });

        mSceneUnload = context.scheduler.AddStage(Stage::SceneUnload, [this](SystemContext &frame)
            {
                mModels->ReleaseResources();
                frame.world.Resource<ModelDrawItems>().items.clear();
                frame.world.Resource<ModelMorphOverrides>().instances.clear();
            });

        // the new cook's models prepare in the background while the old ones keep drawing
        mReloadCommit = context.scheduler.AddStage(Stage::ReloadCommit, [this](SystemContext &)
            {
                mModels->CommitReload();
            });
    }
}
