/**
 * @file PrefabInstantiate.h
 * @author Jackey Cen
 * @brief Builds an independent entity graph from a compiled prefab.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/CompiledPrefab.h"

#include "core/ecs/Entity.h"

#include <map>

namespace mir
{
    class World;

    struct PrefabInstantiation
    {
        Entity mRoot;
        std::map<PrefabNodeAddress, Entity> mEntities;
    };

    PrefabInstantiation InstantiatePrefab(World &world, const CompiledPrefab &prefab, Entity parent = kNullEntity);
}
