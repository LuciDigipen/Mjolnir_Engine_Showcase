/**
 * @file CreateGameProject.h
 * @author sumin.park
 * @brief Writes a new game directory from the editor's New Project dialog.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <string>

namespace mir
{
    // Writes games/<name>/: game.json, an EmptyScene-shaped scenes/default/scene.json
    bool CreateGameProject(const std::string &name, bool useRayTracing);
}
