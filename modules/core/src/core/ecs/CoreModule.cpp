/**
 * @file CoreModule.cpp
 * @author sumin.park
 * @brief Core's components, ECS resources and transform propagation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/ecs/CoreModule.h"

#include "core/ecs/ComponentFieldChanges.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/DeferredAccess.h"
#include "core/ecs/EngineMode.h"
#include "core/ecs/Requests.h"
#include "core/ecs/SceneLoadGroup.h"
#include "core/ecs/SystemScheduler.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"

namespace mir
{
    CoreModule::CoreModule(ModuleContext context, CommandBuffer &commands)
    {
        RegisterCoreComponents();

        context.world.EmplaceResource<EngineModeState>();
        context.world.EmplaceResource<ComponentFieldListeners>();
        context.world.EmplaceResource<FrameCommands>(FrameCommands{ &commands });
        context.world.EmplaceResource<RequestQueue>();
        context.world.EmplaceResource<SceneLoadGroup>();
        mSceneLoad = context.scheduler.AddStage(Stage::SceneLoad,
            [](SystemContext &frame) { frame.world.Resource<SceneLoadGroup>().Begin(); });

        context.scheduler.AddSystem<TransformPropagateSystem>(SystemPhase::Propagate,
            SystemOrder{SystemOrder::kNormal});
    }
}
