/**
 * @file PhysicsModule.cpp
 * @author sumin.park
 * @brief Physics components, the Jolt world and the physics systems.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsModule.h>

#include <physics/CollisionEventSystem.h>
#include <physics/CollisionEvents.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsLayerSettings.h>
#include <physics/PhysicsInterpolationSystem.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

namespace mir
{
    PhysicsModule::PhysicsModule(ModuleContext context, const std::filesystem::path &projectSettingsPath)
    {
        RegisterPhysicsComponents();

        PhysicsWorld &physics = context.world.EmplaceResource<PhysicsWorld>();
        physics.SetLayerTable(ReadPhysicsLayers(projectSettingsPath));
        context.world.EmplaceResource<CollisionEvents>();

        // once per fixed substep, before Propagate turns the Transforms it writes into world matrices
        context.scheduler.AddSystem<PhysicsSystem>(SystemPhase::FixedUpdate, SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<PhysicsInterpolationSystem>(SystemPhase::PostUpdate,
            SystemOrder{SystemOrder::kNormal});
        context.scheduler.AddSystem<CollisionEventSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});

        mHooks = context.world.SetComponentHooks<RigidBody>(
            { .onAdd = &PhysicsSystem::OnRigidBodyAdded, .onRemove = &PhysicsSystem::OnRigidBodyRemoved });

        mPlayBegin = context.scheduler.AddStage(Stage::PlayBegin,
            [](SystemContext &frame) { PhysicsSystem::OnPlayBegin(frame.world); });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd,
            [](SystemContext &frame) { PhysicsSystem::OnPlayEnd(frame.world); });
    }
}
