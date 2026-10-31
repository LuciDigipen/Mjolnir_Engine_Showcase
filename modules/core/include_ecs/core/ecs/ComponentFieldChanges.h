/**
 * @file ComponentFieldChanges.h
 * @author Jackey Cen
 * @brief Tells component owners which reflected fields a commit writes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "Entity.h"
#include "TypeId.h"

#include <algorithm>
#include <span>
#include <string>
#include <vector>

namespace mir
{
    class World;

    // Only the reflected fields that a commit actually wrote. Adds, removes and
    // destroys reach the owner through its component hooks instead.
    struct ComponentFieldChange
    {
        Entity mEntity;
        TypeId mType{};
        std::vector<std::string> mFields;
    };

    using ComponentFieldCallback = void (*)(World &world, std::span<const ComponentFieldChange> changes, void *user);

    struct ComponentFieldHook
    {
        ComponentFieldCallback fn = nullptr;
        void *user = nullptr;

        bool operator==(const ComponentFieldHook &) const = default;
    };

    // Ordered callbacks for a commit's BeforeChange and AfterChange phases.
    class ComponentFieldListeners
    {
    public:
        void AddBeforeChange(ComponentFieldCallback fn, void *user = nullptr)
        {
            Add(mBeforeChange, fn, user);
        }

        void RemoveBeforeChange(ComponentFieldCallback fn, void *user = nullptr)
        {
            Remove(mBeforeChange, fn, user);
        }

        void AddAfterChange(ComponentFieldCallback fn, void *user = nullptr)
        {
            Add(mAfterChange, fn, user);
        }

        void RemoveAfterChange(ComponentFieldCallback fn, void *user = nullptr)
        {
            Remove(mAfterChange, fn, user);
        }

        void NotifyBeforeChange(World &world, std::span<const ComponentFieldChange> changes) const
        {
            Notify(mBeforeChange, world, changes);
        }

        void NotifyAfterChange(World &world, std::span<const ComponentFieldChange> changes) const
        {
            Notify(mAfterChange, world, changes);
        }

    private:
        static void Add(std::vector<ComponentFieldHook> &hooks, ComponentFieldCallback fn, void *user)
        {
            const ComponentFieldHook hook{ fn, user };
            if (std::find(hooks.begin(), hooks.end(), hook) == hooks.end())
                hooks.push_back(hook);
        }

        static void Remove(std::vector<ComponentFieldHook> &hooks, ComponentFieldCallback fn, void *user)
        {
            std::erase(hooks, ComponentFieldHook{ fn, user });
        }

        static void Notify(const std::vector<ComponentFieldHook> &hooks, World &world,
            std::span<const ComponentFieldChange> changes)
        {
            for (const ComponentFieldHook &hook : hooks)
                hook.fn(world, changes, hook.user);
        }

        std::vector<ComponentFieldHook> mBeforeChange;
        std::vector<ComponentFieldHook> mAfterChange;
    };
}
