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

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
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

        // The field's value in the backend's encoding.
        virtual std::string EncodeField(FieldKind kind, const void *bytes) = 0;

        // One command's entries; a refused action carries a reason instead.
        virtual AuthoredAction Revert(const AuthoredKey &key) = 0;
        virtual AuthoredAction Apply(const AuthoredKey &key) = 0;
    };
}
