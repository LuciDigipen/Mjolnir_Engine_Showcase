/**
 * @file PrefabAuthoring.h
 * @author Jackey Cen
 * @brief Authored field, component and node state of prefab documents and scene instances.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "prefab/PrefabCoordinator.h"
#include "prefab/PrefabOverrides.h"
#include "scene/SceneAsset.h"

#include <core/ecs/ComponentFields.h>
#include <core/ecs/Entity.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class World;

    struct PrefabAuthoredKey
    {
        std::optional<PrefabId> mPrefab; // empty names the coordinator's bound scene
        SceneNodeAddress mAddress;
        std::string mComponent;
        std::string mField;

        bool operator==(const PrefabAuthoredKey &) const = default;
    };

    struct PrefabAuthoredEntry
    {
        PrefabAuthoredKey mKey;
        std::optional<std::string> mValue; // JSON text; empty is absent or inherited
    };

    struct PrefabAuthoredWrite
    {
        bool mApplied = false;

        // the bound scene keeps a change Undo does not reverse
        bool mSceneMaintained = false;
    };

    struct PrefabAuthoredAction
    {
        std::vector<PrefabAuthoredEntry> mEntries;
        std::string mRefusal; // non-empty: the action is refused
    };

    // Editor-independent authored state over the coordinator's documents, its bound scene and the World.
    class PrefabAuthoring
    {
    public:
        PrefabAuthoring(World &world, PrefabCoordinator &coordinator);

        // Every key, or none when a key names a missing document, node or component.
        std::optional<std::vector<PrefabAuthoredEntry>> Read(std::span<const PrefabAuthoredKey> keys);

        // Validates the whole batch on copies first; a refused batch changes nothing.
        PrefabAuthoredWrite Write(std::span<const PrefabAuthoredEntry> entries);

        std::optional<PrefabAuthoredKey> FieldKey(Entity entity, std::string_view component,
            std::string_view field) const;
        std::optional<PrefabAuthoredKey> ComponentKey(Entity entity, std::string_view component) const;

        PrefabFieldState FieldState(const PrefabAuthoredKey &key) const;
        PrefabComponentState ComponentState(Entity entity, std::string_view component) const;
        std::vector<std::string> RemovedComponents(Entity entity) const;

        // The field's value as the scene field encoding stores it.
        std::string EncodeField(FieldKind kind, const void *bytes) const;

        PrefabAuthoredAction Revert(const PrefabAuthoredKey &key) const;
        PrefabAuthoredAction Apply(const PrefabAuthoredKey &key) const;

    private:
        struct Member
        {
            StableId mRoot = kNullStableId;
            SceneNodeAddress mNode;
            const ScenePrefabInstance *mInstance = nullptr;
            Entity mEntity = kNullEntity;
        };

        std::optional<Member> FindMember(Entity entity) const;

        World &mWorld;
        PrefabCoordinator &mCoordinator;
    };
}
