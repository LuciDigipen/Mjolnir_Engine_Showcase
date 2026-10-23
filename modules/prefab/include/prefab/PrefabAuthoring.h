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
#include <core/math/WorldReal.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class World;
    struct EntityPresetDesc;

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

    // One document an override can be applied into, outermost first.
    struct PrefabApplyTarget
    {
        PrefabId mPrefab{};
        std::string mLabel;
        bool mOverrides = false;
    };

    enum class PrefabInstanceRole : uint8_t
    {
        Ordinary,
        Root,
        Member,
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

        PrefabInstanceRole InstanceRole(Entity entity) const;

        // Every live bound entity: all of them in the scene, a preview's nested occurrences only.
        std::vector<Entity> PrefabLinkedEntities() const;

        std::optional<PrefabAuthoredKey> FieldKey(Entity entity, std::string_view component,
            std::string_view field) const;
        std::optional<PrefabAuthoredKey> ComponentKey(Entity entity, std::string_view component) const;

        PrefabFieldState FieldState(const PrefabAuthoredKey &key) const;
        PrefabComponentState ComponentState(Entity entity, std::string_view component) const;
        std::vector<std::string> RemovedComponents(Entity entity) const;

        // The field's value as the scene field encoding stores it.
        std::string EncodeField(FieldKind kind, const void *bytes) const;

        PrefabAuthoredAction Revert(const PrefabAuthoredKey &key) const;

        // Every layer below the origin that can take the selected override, outermost first.
        std::vector<PrefabApplyTarget> ApplyTargets(const PrefabAuthoredKey &key) const;

        // Writes the selected override into the chosen target and removes only the origin patch.
        PrefabAuthoredAction Apply(const PrefabAuthoredKey &key, std::size_t target = 0) const;

        // The document preview entities resolve against; empty authors through the bound scene.
        void SetActiveDocument(std::optional<PrefabId> prefab) { mActiveDocument = prefab; }
        std::optional<PrefabId> ActiveDocument() const { return mActiveDocument; }

        std::optional<PrefabNodeId> NodeIdOf(Entity entity) const;

        // One command each, on the active document; the editor wraps the entries in History.
        // A preset fills the new node with the components it creates on a fresh entity.
        PrefabAuthoredAction CreateNode(PrefabNodeId parent, std::size_t siblingIndex,
            const EntityPresetDesc *preset = nullptr);
        PrefabAuthoredAction PlacePrefabNode(PrefabNodeId parent, PrefabId prefab, std::size_t siblingIndex,
            std::optional<WorldVec3> position = std::nullopt);
        PrefabAuthoredAction DeleteNodes(std::span<const PrefabNodeId> nodes);
        PrefabAuthoredAction ReparentNode(PrefabNodeId node, PrefabNodeId parent, std::size_t siblingIndex);

        PrefabAuthoredAction ReparentNodes(std::span<const PrefabNodeId> nodes, PrefabNodeId parent,
            std::size_t siblingIndex);

        // True when the component lives directly in the active document.
        bool WritesComponentDirectly(Entity entity, std::string_view component) const;

    private:
        struct Member
        {
            StableId mRoot = kNullStableId;
            SceneNodeAddress mNode;
            const ScenePrefabInstance *mInstance = nullptr;
            Entity mEntity = kNullEntity;
            std::optional<PrefabId> mPrefab;
        };

        struct Context
        {
            World *mWorld = nullptr;
            LoadedScene *mScene = nullptr;
            std::optional<PrefabId> mPrefab;
        };

        std::optional<Context> CurrentContext() const;
        std::optional<Member> FindMember(Entity entity) const;
        const PrefabDocument *ActivePrefabDocument() const;

        World &mWorld;
        PrefabCoordinator &mCoordinator;
        std::optional<PrefabId> mActiveDocument;
    };
}
