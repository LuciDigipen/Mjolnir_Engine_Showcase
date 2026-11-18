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

#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <algorithm>
#include <utility>

namespace mir
{
    FlightModule::FlightModule(ModuleContext context, AircraftConfigStore::Source source) : mWorld(&context.world)
    {
        RegisterFlightComponents();
        // before PhysicsSystem (kNormal), so its edit sync sees the mass properties on the same step
        context.scheduler.AddSystem<AircraftConfigSystem>(SystemPhase::FixedUpdate,
            SystemOrder{SystemOrder::kEarly}, std::move(source));

        PhysicsStepGates *gates = context.world.TryResource<PhysicsStepGates>();
        if (!gates)
            gates = &context.world.EmplaceResource<PhysicsStepGates>();
        gates->mGates.push_back(&mGate);
    }

    FlightModule::~FlightModule()
    {
        if (PhysicsStepGates *gates = mWorld->TryResource<PhysicsStepGates>())
            std::erase(gates->mGates, static_cast<PhysicsStepGate *>(&mGate));
    }
}
