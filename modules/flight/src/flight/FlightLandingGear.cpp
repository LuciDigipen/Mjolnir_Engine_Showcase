/**
 * @file FlightLandingGear.cpp
 * @author Jackey Cen
 * @brief Runs the raycast gear springs, the wheel friction and the touchdown detection.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/FlightLandingGear.h>

#include <flight/AircraftConfig.h>
#include <flight/AircraftRuntime.h>
#include <flight/FlightFrames.h>
#include <flight/components/Aircraft.h>

#include <core/math/WorldCoordinates.h>
#include <physics/PhysicsWorld.h>

#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

namespace mir
{
    namespace
    {
        float Sign(float value)
        {
            return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
        }

        float Fade(float speed)
        {
            return std::min(std::abs(speed) / kGearFrictionFadeSpeed, 1.0f);
        }
    }

    void StepLandingGear(const AircraftConfig &config, WorldPosition origin, PhysicsWorld &physics, BodyHandle body,
        AircraftRuntime &runtime, float)
    {
        LandingGearState &gearState = runtime.gear;
        gearState.stepTouchdowns.Clear();
        gearState.force = glm::vec3{0.0f};
        gearState.moment = glm::vec3{0.0f};

        const AircraftState &state = runtime.state;
        const glm::quat toLocal = glm::inverse(state.rotation);
        const glm::vec3 rayBody{0.0f, 0.0f, 1.0f};
        const glm::vec3 rayWorld = state.rotation * BodyToLocal(rayBody);
        const BodyHandle excluded[1] = {body};
        RayFilter filter;
        filter.mExclude = excluded;

        for (std::size_t i = 0; i < kMaxGear; ++i)
        {
            GearPointState &point = gearState.points[i];
            if (i >= config.gear.size())
            {
                point = {};
                continue;
            }

            const AircraftConfig::Gear &gear = config.gear[i];
            const std::optional<WorldPosition> mount = WorldPosition::FromMeters(
                origin.Meters() + WorldVec3(state.rotation * BodyToLocal(gear.mount)));
            std::optional<RayHit> hit;
            if (mount)
                hit = physics.RayCast(*mount, rayWorld, gear.restLength, filter);

            const std::optional<WorldOffset> offset = hit ? TryWorldDifference(hit->mPoint, origin) : std::nullopt;
            const std::optional<glm::vec3> contactOffset = offset ? TryRendererLocalFloat(*offset) : std::nullopt;
            if (!hit || !contactOffset)
            {
                point.inContact = false;
                point.compression = 0.0f;
                point.normalForce = 0.0f;
                continue;
            }

            const glm::vec3 contactBody = LocalToBody(toLocal * *contactOffset);
            const glm::vec3 arm = contactBody - config.centerOfMass;
            const glm::vec3 normal = glm::normalize(LocalToBody(toLocal * hit->mNormal));
            const float compression = gear.restLength - hit->mDistance;
            const glm::vec3 contactVelocity =
                state.linearVelocityBody + glm::cross(state.angularVelocityBody, arm);
            const float closingSpeed = glm::dot(contactVelocity, rayBody);
            const float normalMagnitude = std::max(0.0f, gear.stiffness * compression + gear.damping * closingSpeed);

            glm::vec3 force = normal * normalMagnitude;
            // Body X projected on the ground plane; undefined only with the nose straight up or down.
            const glm::vec3 planeForward = glm::vec3{1.0f, 0.0f, 0.0f} - normal * normal.x;
            if (glm::dot(planeForward, planeForward) > 1e-6f)
            {
                glm::vec3 forward = glm::normalize(planeForward);
                if (gear.maxSteer > 0.0f && config.actuators.rudder.maxDeflection > 0.0f)
                {
                    // Turning about body down points the wheel right for a positive rudder.
                    const float steer =
                        runtime.actuators.rudder / config.actuators.rudder.maxDeflection * gear.maxSteer;
                    forward = glm::angleAxis(steer, -normal) * forward;
                }

                const glm::vec3 lateral = glm::cross(forward, normal);
                const float forwardSpeed = glm::dot(contactVelocity, forward);
                const float lateralSpeed = glm::dot(contactVelocity, lateral);
                const float rolling = gear.rollingFriction + (runtime.actuators.brake ? gear.brake : 0.0f);

                force -= forward * (Sign(forwardSpeed) * Fade(forwardSpeed) * rolling * normalMagnitude);
                force -= lateral * (Sign(lateralSpeed) * Fade(lateralSpeed) * gear.sideFriction * normalMagnitude);
            }

            runtime.accumulator.force += force;
            runtime.accumulator.moment += glm::cross(arm, force);
            gearState.force += force;
            gearState.moment += glm::cross(arm, force);

            const bool firstContact = !point.inContact;
            point.inContact = true;
            point.compression = compression;
            point.normalForce = normalMagnitude;
            point.contactBody = contactBody;

            if (firstContact)
                gearState.stepTouchdowns.Push(Touchdown{static_cast<int>(i), closingSpeed, compression});
        }
    }

    std::span<const Touchdown> AircraftTouchdowns(const Aircraft &aircraft)
    {
        const TouchdownList &touchdowns = aircraft.runtime.touchdowns;
        return std::span<const Touchdown>(touchdowns.events, touchdowns.count);
    }
}
