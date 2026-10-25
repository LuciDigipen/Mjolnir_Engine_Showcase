/**
 * @file PrefabInstantiate.cpp
 * @author Jackey Cen
 * @brief Builds an independent entity graph from a compiled prefab.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "prefab/PrefabInstantiate.h"

#include "core/ecs/ComponentFields.h"
#include "core/ecs/ComponentRegistry.h"
#include "core/ecs/TransformHierarchy.h"
#include "core/ecs/World.h"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace mir
{
    PrefabInstantiation InstantiatePrefab(World &world, const CompiledPrefab &prefab, Entity parent)
    {
        PrefabInstantiation result;

        const std::span<const CompiledPrefabNode> nodes = prefab.Nodes();
        std::vector<Entity> entities;
        entities.reserve(nodes.size());

        for (const CompiledPrefabNode &node : nodes)
        {
            const Entity entity = world.CreateEntity();
            entities.push_back(entity);
            result.mEntities.emplace(node.mAddress, entity);

            for (const CompiledPrefabComponent &component : node.mComponents)
                component.mOps->AddCopy(world, entity, component.mBytes.data());
        }

        if (entities.empty())
            return result;

        result.mRoot = entities.front();

        for (std::size_t i = 0; i < nodes.size(); ++i)
        {
            if (nodes[i].mParent.has_value())
            {
                SetParent(world, entities[i], entities[*nodes[i].mParent], kNullEntity, false);
            }
            else if (!parent.IsNull())
            {
                SetParent(world, entities[i], parent, kNullEntity, false);
            }
        }

        for (const PrefabEntityRefRelocation &relocation : prefab.Relocations())
        {
            const CompiledPrefabComponent &component =
                nodes[relocation.mNode].mComponents[relocation.mComponent];
            void *value = component.mOps->GetComponent(world, entities[relocation.mNode]);
            if (value == nullptr)
                continue;

            const FieldDesc &field = component.mOps->mFields[relocation.mField];
            const Entity target = entities[relocation.mTargetNode];
            field.Write(value, &target);
        }

        return result;
    }
}
