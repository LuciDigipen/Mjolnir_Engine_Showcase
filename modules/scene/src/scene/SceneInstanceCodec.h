/**
 * @file SceneInstanceCodec.h
 * @author Jackey Cen
 * @brief Scene prefab instance <-> Json converter and record patch helpers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "scene/SceneAsset.h"
#include "scene/SceneInstancePatch.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace mir
{
    nlohmann::json InstanceToJson(const ScenePrefabInstance &instance);

    // Nullopt when a required field is missing or malformed; never throws.
    std::optional<ScenePrefabInstance> InstanceFromJson(const nlohmann::json &file, std::string_view context);
}
