/**
 * @file SceneInstancePatch.h
 * @author Jackey Cen
 * @brief Query and edit the local patches of a scene prefab instance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "scene/SceneAsset.h"

#include <string_view>

namespace mir
{
    SceneInstancePatch *FindInstancePatch(ScenePrefabInstance &instance, const SceneNodeAddress &target);
    const SceneInstancePatch *FindInstancePatch(const ScenePrefabInstance &instance, const SceneNodeAddress &target);

    SceneInstancePatch &GetOrAddInstancePatch(ScenePrefabInstance &instance, const SceneNodeAddress &target);

    SceneFieldPatch *FindInstanceFieldPatch(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field);
    const SceneFieldPatch *FindInstanceFieldPatch(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field);

    // Removes the field patch and an otherwise empty patch entry.
    bool EraseInstanceFieldPatch(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field);

    SceneComponentRecord *FindAddedComponent(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component);
    const SceneComponentRecord *FindAddedComponent(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component);

    // Removes the local addition and an otherwise empty patch entry.
    bool EraseAddedComponent(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component);

    bool HasRemovedComponent(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component);

    // Removes the removal record and an otherwise empty patch entry.
    bool EraseRemovedComponent(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component);
}
