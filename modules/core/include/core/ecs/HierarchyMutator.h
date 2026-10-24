/**
 * @file HierarchyMutator.h
 * @author sumin.park
 * @brief Header to share HierarchyIndex's private mutators with world(only)
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "core/ecs/HierarchyIndex.h"

#include <vector>

namespace mir::detail
{
    // Only World (create/destroy) and TransformHierarchy (reparent) go through here
    struct HierarchyMutator
    {
        static bool SetParent(HierarchyIndex &index, Entity child, Entity parent, Entity before)
        {
            return index.SetParent(child, parent, before);
        }

        static bool MoveToLastSibling(HierarchyIndex &index, Entity entity)
        {
            return index.MoveToLastSibling(entity);
        }

        static void Remove(HierarchyIndex &index, Entity entity) { index.Remove(entity); }

        static std::vector<Entity> DetachChildren(HierarchyIndex &index, Entity entity)
        {
            return index.DetachChildren(entity);
        }

        static void AddRoot(HierarchyIndex &index, Entity entity) { index.AddRoot(entity); }
    };
}
