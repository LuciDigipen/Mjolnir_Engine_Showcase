/**
 * @file FlightModule.cpp
 * @author Jackey Cen
 * @brief Flight components and the step gate that flies them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightModule.h>

#include <flight/AircraftConfigSystem.h>
#include <flight/ComponentRegistration.h>
#include <flight/FlightCameraSystem.h>
#include <flight/FlightEnvironment.h>
#include <flight/FlightInputSystem.h>
#include <flight/FlightTouchdownSystem.h>

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <algorithm>
#include <utility>

namespace mir
{
    FlightModule::FlightModule(ModuleContext context, AircraftConfigStore::Source source,
        std::function<AircraftConfigStore::Source()> reloadSource)
        : mWorld(&context.world), mStore(std::move(source)), mReloadSource(std::move(reloadSource))
    {
        RegisterFlightComponents();
        // before PhysicsSystem (kNormal), so its edit sync sees the mass properties on the same step
        context.scheduler.AddSystem<AircraftConfigSystem>(SystemPhase::FixedUpdate,
            SystemOrder{SystemOrder::kEarly}, mStore);

        // Update runs after PreUpdate's InputSystem and before any FixedUpdate substep
        context.scheduler.AddSystem<FlightInputSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});

        // sets the cameras up from the loaded config and switches the view on the camera toggle
        context.scheduler.AddSystem<FlightCameraSystem>(SystemPhase::Update, SystemOrder{SystemOrder::kNormal});

        // once per frame, after every fixed step, so scripts see the frame's touchdowns in Update
        context.scheduler.AddSystem<FlightTouchdownSystem>(SystemPhase::LateUpdate,
            SystemOrder{SystemOrder::kNormal});

        if (mReloadSource)
        {
            mReloadStages[0] = context.scheduler.AddStage(Stage::ReloadPrepare,
                [this](SystemContext &) { mStore.PrepareReload(mReloadSource()); });
            mReloadStages[1] = context.scheduler.AddStage(Stage::ReloadCommit,
                [this](SystemContext &) { mStore.CommitReload(); });
            mReloadStages[2] = context.scheduler.AddStage(Stage::ReloadAbort,
                [this](SystemContext &) { mStore.AbortReload(); });
        }

        if (!context.world.TryResource<FlightEnvironment>())
            context.world.EmplaceResource<FlightEnvironment>();

        context.world.EmplaceResource<AircraftConfigStoreRef>(AircraftConfigStoreRef{ &mStore });

        PhysicsStepGates *gates = context.world.TryResource<PhysicsStepGates>();
        if (!gates)
            gates = &context.world.EmplaceResource<PhysicsStepGates>();
        gates->mGates.push_back(&mGate);
    }

    FlightModule::~FlightModule()
    {
        if (PhysicsStepGates *gates = mWorld->TryResource<PhysicsStepGates>())
            std::erase(gates->mGates, static_cast<PhysicsStepGate *>(&mGate));
        mWorld->RemoveResource<AircraftConfigStoreRef>();
    }
}
