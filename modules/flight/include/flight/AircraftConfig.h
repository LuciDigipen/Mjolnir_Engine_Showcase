/**
 * @file AircraftConfig.h
 * @author Jackey Cen
 * @brief The values a *.aircraft.json file holds, in body frame and SI units.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetDecoder.h>

#include <flight/AerodynamicModel.h>
#include <flight/FlightCurve.h>

#include <glm/trigonometric.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    inline constexpr std::size_t kMaxEngines = 4;
    inline constexpr std::size_t kMaxGear = 8;

    struct AircraftConfig
    {
        struct Reference
        {
            float area = 0.0f;
            float span = 0.0f;
            float chord = 0.0f;
        };

        struct Camera
        {
            glm::vec3 eye{0.0f};
            float chaseDistance = 0.0f;
            float chaseHeight = 0.0f;
        };

        struct Node
        {
            std::string component;
            std::string model;
        };

        struct Actuator
        {
            float maxDeflection = 0.0f; // radians
            float rate = 0.0f;          // radians per second
            float delay = 0.0f;         // seconds
        };

        struct Flaps
        {
            std::vector<float> detents; // radians, strictly increasing
            float rate = 0.0f;          // radians per second
        };

        struct Trim
        {
            float step = 0.0f; // radians
            float max = 0.0f;  // radians
        };

        struct Actuators
        {
            Actuator elevator;
            Actuator aileron;
            Actuator rudder;
            Flaps flaps;
            Trim trim;
        };

        struct Engine
        {
            glm::vec3 position{0.0f};
            glm::vec3 direction{0.0f}; // body frame, normalised on load
            float idleRpm = 0.0f;
            float maxRpm = 0.0f;
            float rpmTau = 0.0f;                    // seconds
            std::vector<CurvePoint> staticThrust;   // [rpmFraction, newtons]
            std::vector<CurvePoint> speedFactor;    // [axial m/s, factor]
        };

        struct Gear
        {
            std::string name;
            glm::vec3 mount{0.0f}; // body frame, metres from the entity origin
            float restLength = 0.0f;
            float stiffness = 0.0f; // newtons per metre
            float damping = 0.0f; // newton seconds per metre
            float rollingFriction = 0.0f;
            float sideFriction = 0.0f;
            float brake = 0.0f; // friction coefficient while the brake is held
            float maxSteer = 0.0f; // radians; above zero steers with the rudder
        };

        int version = 0;
        float mass = 0.0f;
        glm::vec3 inertiaDiagonal{0.0f};
        // Ixy, Ixz, Iyz. The RigidBody tensor holds their negatives.
        glm::vec3 inertiaProducts{0.0f};
        glm::vec3 centerOfMass{0.0f};
        Reference reference;
        Camera camera;
        std::vector<Node> nodes;
        float throttleRate = 0.0f; // per second
        // Below this margin to the L2 critical angle the stall warning is on.
        float stallWarningMargin = glm::radians(5.0f);
        Actuators actuators;
        std::vector<Engine> engines;
        std::vector<Gear> gear;
        AerodynamicProfile aero;
    };

    // On failure error is "<json path>: <reason>", or the parser message for malformed JSON.
    bool ParseAircraftConfig(std::string_view json, AircraftConfig &out, std::string &error);

    const AssetDecoder<AircraftConfig> &AircraftConfigDecoder();
}
