/**
 * @file FlightLandingGear.h
 * @author Jackey Cen
 * @brief The landing gear stage: a spring-damper force and friction under each wheel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftRuntime.h>

#include <core/math/WorldCoordinates.h>
#include <physics/PhysicsTypes.h>

#include <span>

namespace mir
{
    struct Aircraft;
    struct AircraftConfig;
    class PhysicsWorld;

    // Below this the friction fades out with the speed, so a parked aircraft does not jitter.
    inline constexpr float kGearFrictionFadeSpeed = 0.5f;

    // Raycasts every gear down the body and adds its force and the moment about the centre of mass.
    void StepLandingGear(const AircraftConfig &config, WorldPosition origin, PhysicsWorld &physics, BodyHandle body,
        AircraftRuntime &runtime, float dt);

    // The previous frame's touchdown events; empty until the publish system has run.
    std::span<const Touchdown> AircraftTouchdowns(const Aircraft &aircraft);
}
