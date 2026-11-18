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

#include <glm/vec3.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace mir
{
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

        int version = 0;
        float mass = 0.0f;
        glm::vec3 inertiaDiagonal{0.0f};
        // Ixy, Ixz, Iyz. The RigidBody tensor holds their negatives.
        glm::vec3 inertiaProducts{0.0f};
        glm::vec3 centerOfMass{0.0f};
        Reference reference;
        Camera camera;
        std::vector<Node> nodes;
    };

    // On failure error is "<json path>: <reason>", or the parser message for malformed JSON.
    bool ParseAircraftConfig(std::string_view json, AircraftConfig &out, std::string &error);

    const AssetDecoder<AircraftConfig> &AircraftConfigDecoder();
}
