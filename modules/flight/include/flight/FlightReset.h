/**
 * @file FlightReset.h
 * @author Jackey Cen
 * @brief Puts an aircraft back at a pose with zeroed controls, gear state and engines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <glm/gtc/quaternion.hpp>

namespace mir
{
    class World;
    struct Entity;

    // False unless the entity has Aircraft, a live dynamic RigidBody and a loaded config.
    bool ResetAircraft(World &world, Entity entity, WorldPosition position, glm::quat rotation, float airspeed,
        float throttle);
}
