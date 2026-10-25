/**
 * @file PrefabAuthoringAdapter.h
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
#include <prefab/PrefabSave.h>

#include <cstddef>
#include <filesystem>
#include <functional>
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
        PrefabAuthoringAdapter(World &world, PrefabCoordinator &coordinator,
            std::filesystem::path projectRoot, std::filesystem::path prefabsDir);

        std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) override;
        AuthoringWrite Write(std::span<const AuthoredEntry> entries) override;

        std::optional<AuthoredKey> KeyOf(Entity entity, std::string_view component, std::string_view field) override;
        std::optional<AuthoredKey> ComponentKeyOf(Entity entity, std::string_view component) override;

        AuthoredFieldState FieldState(const AuthoredKey &key) override;
        AuthoredComponentState ComponentState(Entity entity, std::string_view component) override;
        std::vector<std::string> RemovedComponents(Entity entity) override;

        AuthoredInstanceRole InstanceRole(Entity entity) override;
        std::vector<Entity> PrefabLinkedEntities() override;

        std::string EncodeField(FieldKind kind, const void *bytes) override;

        AuthoredAction Revert(const AuthoredKey &key) override;
        AuthoredAction Apply(const AuthoredKey &key) override;
        AuthoredAction Apply(const AuthoredKey &key, std::size_t target) override;
        std::vector<AuthoredApplyTarget> ApplyTargets(const AuthoredKey &key) override;

        AuthoredAction CreateNode(Entity parent, std::size_t siblingIndex,
            const EntityPresetDesc *preset = nullptr) override;
        AuthoredAction PlacePrefabNode(std::string_view contentPath, Entity parent, std::size_t siblingIndex,
            const std::optional<WorldVec3> &placement) override;
        AuthoredAction DeleteNodes(std::span<const Entity> nodes) override;
        AuthoredAction ReparentNode(Entity node, Entity parent, std::size_t siblingIndex) override;
        AuthoredAction CreatePrefab(Entity entity) override;

        AuthoredAction ReparentNodes(std::span<const Entity> nodes, Entity parent, std::size_t siblingIndex) override;

        bool WritesComponentDirectly(Entity entity, std::string_view component) override;

        void SetActiveDocument(std::optional<PrefabId> prefab) { mAuthoring.SetActiveDocument(prefab); }
        std::optional<PrefabId> ActiveDocument() const { return mAuthoring.ActiveDocument(); }

        bool SaveScenePrefabs(const LoadedScene &scene, const PrefabUnsavedPredicate &unsaved,
            const std::function<void(PrefabId)> &saved);

    private:
        std::optional<std::string> DocumentValue(PrefabId id) const;
        bool ApplyDocumentEntry(const AuthoredEntry &entry);

        World &mWorld;
        PrefabCoordinator &mCoordinator;
        PrefabAuthoring mAuthoring;
        std::filesystem::path mProjectRoot;
        std::filesystem::path mPrefabsDir;
    };
}
