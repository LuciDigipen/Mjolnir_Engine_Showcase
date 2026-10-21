/**
 * @file EntityChanges.h
 * @author Jackey Cen
 * @brief Describes one batch of entity changes and notifies resource owners.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "Entity.h"
#include "TypeId.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    class World;

    struct EntityComponentRef
    {
        Entity mEntity;
        TypeId mType{};

        bool operator==(const EntityComponentRef &) const = default;
    };

    struct EntityComponentChange
    {
        Entity mEntity;
        TypeId mType{};
        std::vector<std::string> mFields;
    };

    // One commit's structural and field changes, before or after the World is
    // mutated.
    struct EntityChangeSet
    {
        std::vector<Entity> mRemovedEntities;

        std::vector<EntityComponentRef> mRemovedComponents;

        // Only the reflected fields that a commit actually wrote.
        std::vector<EntityComponentChange> mChangedComponents;

        std::vector<Entity> mAddedEntities;

        std::vector<EntityComponentRef> mAddedComponents;
    };

    struct EntityChangeHook
    {
        void (*fn)(World &world, const EntityChangeSet &changes, void *user) = nullptr;
        void *user = nullptr;

        bool operator==(const EntityChangeHook &) const = default;
    };

    // Ordered callbacks for a commit's BeforeRemove and AfterChange phases.
    class EntityChangeListeners
    {
    public:
        void AddBeforeRemove(void (*fn)(World &, const EntityChangeSet &, void *), void *user = nullptr)
        {
            Add(mBeforeRemove, fn, user);
        }

        void RemoveBeforeRemove(void (*fn)(World &, const EntityChangeSet &, void *), void *user = nullptr)
        {
            Remove(mBeforeRemove, fn, user);
        }

        void AddAfterChange(void (*fn)(World &, const EntityChangeSet &, void *), void *user = nullptr)
        {
            Add(mAfterChange, fn, user);
        }

        void RemoveAfterChange(void (*fn)(World &, const EntityChangeSet &, void *), void *user = nullptr)
        {
            Remove(mAfterChange, fn, user);
        }

        void NotifyBeforeRemove(World &world, const EntityChangeSet &changes) const
        {
            Notify(mBeforeRemove, world, changes);
        }

        void NotifyAfterChange(World &world, const EntityChangeSet &changes) const
        {
            Notify(mAfterChange, world, changes);
        }

    private:
        static void Add(std::vector<EntityChangeHook> &hooks, void (*fn)(World &, const EntityChangeSet &, void *),
            void *user)
        {
            const EntityChangeHook hook{ fn, user };
            if (std::find(hooks.begin(), hooks.end(), hook) == hooks.end())
                hooks.push_back(hook);
        }

        static void Remove(std::vector<EntityChangeHook> &hooks, void (*fn)(World &, const EntityChangeSet &, void *),
            void *user)
        {
            std::erase(hooks, EntityChangeHook{ fn, user });
        }

        static void Notify(const std::vector<EntityChangeHook> &hooks, World &world, const EntityChangeSet &changes)
        {
            for (const EntityChangeHook &hook : hooks)
                hook.fn(world, changes, hook.user);
        }

        std::vector<EntityChangeHook> mBeforeRemove;
        std::vector<EntityChangeHook> mAfterChange;
    };
}
