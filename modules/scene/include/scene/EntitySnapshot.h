/**
 * @file EntitySnapshot.h
 * @author Sumin Park
 * @brief Capture an entity and its descendants as data, and rebuild them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/Entity.h"
#include "scene/SceneAsset.h"

#include <span>
#include <string>
#include <vector>

namespace mir
{
    class World;

    struct EntitySnapshot
    {
        std::string mData;
    };

    
    std::vector<Entity> SnapshotMembers(World &world, Entity root);

    EntitySnapshot CaptureEntity(World &world, Entity root);

    Entity RestoreEntity(World &world, const EntitySnapshot &snapshot, LoadedScene &scene,
        Entity parent = kNullEntity);

    //For undo, i gets stableIds[i] back, kNullStableId leaves it unregistered.
    //returns the members in SnapshotMembers order.
    std::vector<Entity> RestoreEntityAt(World &world, const EntitySnapshot &snapshot, LoadedScene &scene,
        std::span<const StableId> stableIds, Entity parent = kNullEntity, Entity before = kNullEntity);

    Entity DuplicateSceneEntity(World &world, LoadedScene &scene, Entity source);
}
