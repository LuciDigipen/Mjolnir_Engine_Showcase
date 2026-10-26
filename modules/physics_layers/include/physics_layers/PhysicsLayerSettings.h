/**
 * @file PhysicsLayerSettings.h
 * @author sumin.park
 * @brief project_settings.json's "physicsLayers" section
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics_layers/PhysicsLayers.h>

#include <filesystem>
#include <string>

namespace mir
{
    // read from json file
    PhysicsLayerTable ReadPhysicsLayers(const std::filesystem::path &settingsPath);

    // write to disk
    bool WritePhysicsLayers(const std::filesystem::path &settingsPath, const PhysicsLayerTable &table);

    // check validity and return error msg
    std::string CheckPhysicsLayers(const PhysicsLayerTable &table);
}
