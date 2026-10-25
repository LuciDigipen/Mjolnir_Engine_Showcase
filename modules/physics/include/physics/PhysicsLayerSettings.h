/**
 * @file PhysicsLayerSettings.h
 * @author sumin.park
 * @brief project_settings.json's "physicsLayers" section: the layer names and which pairs do not collide.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <physics/PhysicsLayers.h>

#include <filesystem>
#include <string>

namespace mir
{
    // A missing file or section is the default table, every pair colliding. Throws SetupError (ProjectData)
    // for a section that is not valid, naming what is wrong.
    PhysicsLayerTable ReadPhysicsLayers(const std::filesystem::path &settingsPath);

    // Rewrites only the section; false, logged, when the file cannot be written.
    bool WritePhysicsLayers(const std::filesystem::path &settingsPath, const PhysicsLayerTable &table);

    // What is wrong with a table a person edited, or empty: a name used twice, a named layer pair that cannot be
    // written, "Default" renamed.
    std::string CheckPhysicsLayers(const PhysicsLayerTable &table);
}
