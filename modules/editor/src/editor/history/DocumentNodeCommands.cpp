/**
 * @file DocumentNodeCommands.cpp
 * @author Jackey Cen
 * @brief Create, delete and reparent a document's nodes as one authored command each.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/DocumentNodeCommands.h>

#include <editor/history/AuthoredView.h>
#include <editor/history/Authoring.h>
#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/HierarchyIndex.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        Entity DocumentRoot(const World &world)
        {
            const HierarchyIndex &index = world.HierarchyIndex();
            if (index.Roots().empty())
                return kNullEntity;
            return index.Roots().front();
        }

        std::size_t SiblingIndex(const World &world, Entity parent, Entity before,
            std::span<const Entity> moving = {})
        {
            const HierarchyIndex &index = world.HierarchyIndex();
            std::vector<Entity> children;
            for (Entity child : index.ChildrenOf(parent))
                if (std::find(moving.begin(), moving.end(), child) == moving.end())
                    children.push_back(child);
            if (before.IsNull())
                return children.size();

            const auto it = std::find(children.begin(), children.end(), before);
            return it == children.end() ? children.size() : static_cast<std::size_t>(std::distance(children.begin(), it));
        }

        std::unique_ptr<ICommand> FromAction(const AuthoredAction &action)
        {
            if (!action.mAllowed)
            {
                MIR_LOG_WARN("Editor: {} refused: {}", action.mName, action.mReason);
                return nullptr;
            }
            return MakeAuthoredCommand(action.mName, action.mEntries);
        }
    }

    std::unique_ptr<ICommand> MakeDocumentEntityCommand(const EntityAction &action, IAuthoredEntities &authored,
        World &world)
    {
        const Entity root = DocumentRoot(world);
        switch (action.mKind)
        {
        case EntityActionKind::Create:
        {
            if (root.IsNull())
                return nullptr;
            return FromAction(authored.CreateNode(root, SiblingIndex(world, root, kNullEntity), action.mPreset));
        }
        case EntityActionKind::Delete:
        {
            const Entity nodes[] = { action.mTarget };
            return FromAction(authored.DeleteNodes(nodes));
        }
        case EntityActionKind::Reparent:
        {
            if (root.IsNull() || action.mTargets.empty())
                return nullptr;

            const Entity parent = action.mParent.IsNull() ? root : action.mParent;
            const std::size_t index = SiblingIndex(world, parent, action.mBefore, action.mTargets);
            return FromAction(authored.ReparentNodes(action.mTargets, parent, index));
        }
        case EntityActionKind::PlacePrefab:
        {
            if (root.IsNull() || action.mPrefabPath.empty())
                return nullptr;

            const Entity parent = action.mParent.IsNull() ? root : action.mParent;
            return FromAction(authored.PlacePrefabNode(action.mPrefabPath, parent,
                SiblingIndex(world, parent, kNullEntity), action.mPlacement));
        }
        case EntityActionKind::Duplicate:
        case EntityActionKind::CopyAsOrdinaryObject:
        case EntityActionKind::None:
            break;
        }
        return nullptr;
    }
}
