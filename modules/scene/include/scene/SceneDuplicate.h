/**
 * @file SceneDuplicate.h
 * @author Jackey Cen
 * @brief Duplicate scene entities, keeping a Prefab instance linked or copying a member as ordinary.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "scene/SceneAsset.h"

#include <optional>

namespace mir
{
    class World;

    // The scene id of a live entity; empty for one the scene does not track.
    std::optional<StableId> StableIdOf(const LoadedScene &scene, Entity entity);

    // The instance that binds member, with its root's id in outRoot; null for an ordinary entity.
    const ScenePrefabInstance *FindInstanceBinding(const LoadedScene &scene, StableId member, StableId &outRoot);

    // An instance root becomes a linked instance of the same Prefab; a non-root
    // member is refused. Every other entity duplicates as before.
    Entity DuplicateSceneEntitySubtree(World &world, LoadedScene &scene, Entity source);

    // The member's effective subtree as ordinary entities beside its instance root.
    Entity CopyInstanceMemberAsOrdinary(World &world, LoadedScene &scene, Entity source);
}
