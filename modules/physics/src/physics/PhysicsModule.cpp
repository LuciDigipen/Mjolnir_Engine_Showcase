/**
 * @file PhysicsModule.cpp
 * @author sumin.park
 * @brief Physics components, the Jolt world and the physics systems.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/PhysicsModule.h>

#include <physics/ColliderDebugSystem.h>
#include <physics/CollisionEventSystem.h>
#include <physics/CollisionEvents.h>
#include <physics/ComponentRegistration.h>
#include <physics/PhysicsInterpolationSystem.h>
#include <physics/PhysicsSystem.h>
#include <physics/PhysicsWorld.h>
#include <physics/components/RigidBody.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <jobs/JobExecutor.h>

namespace mir
{
    PhysicsModule::PhysicsModule(ModuleContext context, JobExecutor *jobs)
    {
        RegisterPhysicsComponents();

        PhysicsWorldDesc desc;
        if (jobs) desc.mWorkerThreads = static_cast<int>(jobs->WorkerCount());
        PhysicsWorld &physics = context.world.EmplaceResource<PhysicsWorld>(desc);
        MIR_LOG_INFO("Physics: {} simulation workers, {} pose workers", physics.WorkerCount(), jobs ? jobs->WorkerCount() : 0);
        physics.SetLayerTable(context.world.Resource<PhysicsLayerTable>());
        context.world.EmplaceResource<CollisionEvents>();

        // once per fixed substep, before Propagate turns the Transforms it writes into world matrices
        context.scheduler.AddSystem<PhysicsSystem>(SystemPhase::FixedUpdate, SystemOrder{ SystemOrder::kNormal }, jobs);
        context.scheduler.AddSystem<PhysicsInterpolationSystem>(SystemPhase::PostUpdate,
            SystemOrder{ SystemOrder::kNormal });
        context.scheduler.AddSystem<CollisionEventSystem>(SystemPhase::LateUpdate, SystemOrder{ SystemOrder::kNormal });
        // after every fixed step and Propagate, so the outlines are this frame's poses
        context.scheduler.AddSystem<ColliderDebugSystem>(SystemPhase::LateUpdate, SystemOrder{ SystemOrder::kNormal });

        mHooks = context.world.SetComponentHooks<RigidBody>(
            { .onAdd = &PhysicsSystem::OnRigidBodyAdded, .onRemove = &PhysicsSystem::OnRigidBodyRemoved });

        mPlayBegin = context.scheduler.AddStage(Stage::PlayBegin,
            [](SystemContext &frame) { PhysicsSystem::OnPlayBegin(frame.world); });
        mPlayEnd = context.scheduler.AddStage(Stage::PlayEnd,
            [](SystemContext &frame) { PhysicsSystem::OnPlayEnd(frame.world); });
    }
}
