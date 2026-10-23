/**
 * @file PrefabAuthoring.h
 * @author Jackey Cen
 * @brief App's adapter between the editor's authored commands and the prefab module's authored state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/AuthoredView.h>
#include <editor/history/Authoring.h>
#include <prefab/PrefabAuthoring.h>

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class PrefabCoordinator;
    class World;

    class PrefabAuthoringAdapter final : public IAuthoringBackend, public IAuthoredEntities
    {
    public:
        PrefabAuthoringAdapter(World &world, PrefabCoordinator &coordinator);

        std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) override;
        AuthoringWrite Write(std::span<const AuthoredEntry> entries) override;

        std::optional<AuthoredKey> KeyOf(Entity entity, std::string_view component, std::string_view field) override;
        std::optional<AuthoredKey> ComponentKeyOf(Entity entity, std::string_view component) override;

        AuthoredFieldState FieldState(const AuthoredKey &key) override;
        AuthoredComponentState ComponentState(Entity entity, std::string_view component) override;
        std::vector<std::string> RemovedComponents(Entity entity) override;

        std::string EncodeField(FieldKind kind, const void *bytes) override;

        AuthoredAction Revert(const AuthoredKey &key) override;
        AuthoredAction Apply(const AuthoredKey &key) override;

    private:
        PrefabAuthoring mAuthoring;
    };
}
