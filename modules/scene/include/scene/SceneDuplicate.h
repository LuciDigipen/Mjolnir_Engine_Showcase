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

namespace mir
{
    class World;

    // An instance root becomes a linked instance of the same Prefab; a non-root
    // member is refused. Every other entity duplicates as before.
    Entity DuplicateSceneEntitySubtree(World &world, LoadedScene &scene, Entity source);

    // The member's effective subtree as ordinary entities beside its instance root.
    Entity CopyInstanceMemberAsOrdinary(World &world, LoadedScene &scene, Entity source);
}
