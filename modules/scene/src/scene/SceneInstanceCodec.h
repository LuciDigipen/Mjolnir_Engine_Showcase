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

#include <nlohmann/json.hpp>

#include <optional>
#include <string_view>

namespace mir
{
    nlohmann::json InstanceToJson(const ScenePrefabInstance &instance);

    // Nullopt when a required field is missing or malformed; never throws.
    std::optional<ScenePrefabInstance> InstanceFromJson(const nlohmann::json &file, std::string_view context);

    SceneInstancePatch &GetOrAddInstancePatch(ScenePrefabInstance &instance, const SceneNodeAddress &target);

    SceneFieldPatch *FindInstanceFieldPatch(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field);

    // Removes the field patch and an otherwise empty patch entry.
    bool EraseInstanceFieldPatch(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field);
}
