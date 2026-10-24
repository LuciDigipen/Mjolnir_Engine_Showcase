/**
 * @file PrefabOverrides.h
 * @author Jackey Cen
 * @brief Field and component overrides of a single-level prefab scene instance.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "prefab/CompiledPrefab.h"
#include "prefab/PrefabRegistry.h"
#include "scene/SceneAsset.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    class World;

    enum class PrefabFieldState : uint8_t
    {
        Inherited,
        Overridden,
    };

    enum class PrefabComponentState : uint8_t
    {
        Absent,
        Inherited,
        Added,
        Removed,
    };

    enum class PrefabPatchKind : uint8_t
    {
        Field,
        AddedComponent,
        RemovedComponent,
    };

    struct PrefabPatchSelection
    {
        SceneNodeAddress mTarget;
        PrefabPatchKind mKind = PrefabPatchKind::Field;
        std::string mComponent;
        std::string mField;
    };

    struct PrefabApplyResult
    {
        bool mApplied = false;
        std::vector<PrefabDiagnostic> mDiagnostics;
    };

    PrefabFieldState InstanceFieldState(const LoadedScene &scene, StableId root, const SceneNodeAddress &node,
        std::string_view component, std::string_view field);

    PrefabComponentState InstanceComponentState(const PrefabRegistry &registry, const LoadedScene &scene,
        StableId root, const SceneNodeAddress &node, std::string_view component);

    // Presence is the override: the value is recorded even when it equals the template value.
    bool SetInstanceFieldOverride(World &world, LoadedScene &scene, const PrefabRegistry &registry, StableId root,
        const SceneNodeAddress &node, std::string_view component, std::string_view field,
        const nlohmann::json &value, std::vector<PrefabDiagnostic> *diagnostics = nullptr);

    bool AddInstanceComponent(World &world, LoadedScene &scene, const PrefabRegistry &registry, StableId root,
        const SceneNodeAddress &node, std::string_view component,
        std::vector<PrefabDiagnostic> *diagnostics = nullptr);

    bool RemoveInstanceComponent(World &world, LoadedScene &scene, const PrefabRegistry &registry, StableId root,
        const SceneNodeAddress &node, std::string_view component,
        std::vector<PrefabDiagnostic> *diagnostics = nullptr);

    // Removes one selected patch and leaves the member at the compiled template value.
    bool RevertInstancePatch(World &world, const PrefabRegistry &registry, CompiledPrefabCache &cache,
        LoadedScene &scene, StableId root, const PrefabPatchSelection &selection,
        std::vector<PrefabDiagnostic> *diagnostics = nullptr);

    // Writes the selected patches into the instance's in-memory prefab document as one batch.
    PrefabApplyResult ApplyPrefabInstancePatches(LoadedScene &scene, PrefabRegistry &registry,
        CompiledPrefabCache &cache, StableId root, std::span<const PrefabPatchSelection> selections);
}
