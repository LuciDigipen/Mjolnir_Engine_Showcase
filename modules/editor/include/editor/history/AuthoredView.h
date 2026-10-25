/**
 * @file AuthoredView.h
 * @author Jackey Cen
 * @brief The editor's view of authored instance overrides, so it never sees a Prefab type.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Authoring.h>

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
    struct EntityPresetDesc;

    enum class AuthoredFieldState : uint8_t
    {
        Inherited,
        Overridden,
    };

    enum class AuthoredComponentState : uint8_t
    {
        Absent,
        Inherited,
        Added,
        Removed,
    };

    enum class AuthoredInstanceRole : uint8_t
    {
        Ordinary,
        Root,
        Member,
    };

    struct AuthoredAction
    {
        bool mAllowed = true;
        std::string mReason;
        std::string mName;
        std::vector<AuthoredEntry> mEntries;
    };

    struct AuthoredApplyTarget
    {
        std::string mLabel;
        bool mOverrides = false;
    };

    class IAuthoredEntities
    {
    public:
        virtual ~IAuthoredEntities() = default;

        // Empty for an ordinary entity and for the instance root's Transform.
        virtual std::optional<AuthoredKey> KeyOf(Entity entity, std::string_view component,
            std::string_view field) = 0;
        virtual std::optional<AuthoredKey> ComponentKeyOf(Entity entity, std::string_view component) = 0;

        virtual AuthoredFieldState FieldState(const AuthoredKey &key) = 0;
        virtual AuthoredComponentState ComponentState(Entity entity, std::string_view component) = 0;
        virtual std::vector<std::string> RemovedComponents(Entity entity) = 0;

        virtual AuthoredInstanceRole InstanceRole(Entity entity) = 0;

        // Every live entity a Prefab occurrence binds in the current context.
        virtual std::vector<Entity> PrefabLinkedEntities() { return {}; }

        // The field's value in the backend's encoding.
        virtual std::string EncodeField(FieldKind kind, const void *bytes) = 0;

        // One command's entries; a refused action carries a reason instead.
        virtual AuthoredAction Revert(const AuthoredKey &key) = 0;
        virtual AuthoredAction Apply(const AuthoredKey &key) = 0;

        // Every layer below the origin that can take the override, outermost first.
        virtual std::vector<AuthoredApplyTarget> ApplyTargets(const AuthoredKey &) { return {}; }

        // The same Apply, written into the chosen layer instead of the outermost one.
        virtual AuthoredAction Apply(const AuthoredKey &key, std::size_t target)
        {
            (void)target;
            return Apply(key);
        }

        // Node edits on the active document; a null parent means the document root.
        // A preset fills the new node with the components it creates on a fresh entity.
        virtual AuthoredAction CreateNode(Entity parent, std::size_t siblingIndex,
            const EntityPresetDesc *preset = nullptr) = 0;
        // Adds a nested occurrence of the content Prefab; placement, when set, is parent-local.
        virtual AuthoredAction PlacePrefabNode(std::string_view contentPath, Entity parent,
            std::size_t siblingIndex, const std::optional<WorldVec3> &placement) = 0;
        virtual AuthoredAction DeleteNodes(std::span<const Entity> nodes) = 0;
        virtual AuthoredAction ReparentNode(Entity node, Entity parent, std::size_t siblingIndex) = 0;

        // The insertion index is measured after all moving nodes are removed.
        virtual AuthoredAction ReparentNodes(std::span<const Entity> nodes, Entity parent, std::size_t siblingIndex)
        {
            AuthoredAction result;
            result.mName = "Reparent Nodes";
            for (Entity node : nodes)
            {
                AuthoredAction action = ReparentNode(node, parent, siblingIndex++);
                if (!action.mAllowed)
                    return action;
                result.mEntries.insert(result.mEntries.end(), action.mEntries.begin(), action.mEntries.end());
            }
            return result;
        }

        // Captures an ordinary scene entity as one new Prefab document.
        virtual AuthoredAction CreatePrefab(Entity entity) = 0;

        // True when removing the component deletes it from the active document
        // instead of hiding one the template provides.
        virtual bool WritesComponentDirectly(Entity entity, std::string_view component) = 0;
    };
}
