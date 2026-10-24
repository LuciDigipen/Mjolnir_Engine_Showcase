/**
 * @file SceneEntityRefs.h
 * @author Jackey Cen
 * @brief Clears authored EntityRefs that name removed scene entities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "scene/SceneAsset.h"

#include <core/ecs/TypeId.h>

#include <span>
#include <string>
#include <vector>

namespace mir
{
    class World;

    struct ClearedSceneEntityRef
    {
        Entity mEntity;
        TypeId mType{};
        std::string mField;
    };

    // Writes null over every reflected EntityRef that names one of removedIds and
    // records an explicit null patch for an instance member. Marks the scene dirty
    // when authored data changed. Entities in removedIds are not scanned.
    bool ClearSceneEntityRefsTo(World &world, LoadedScene &scene, std::span<const StableId> removedIds,
        std::vector<ClearedSceneEntityRef> *cleared = nullptr);
}
