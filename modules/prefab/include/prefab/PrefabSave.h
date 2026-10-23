/**
 * @file PrefabSave.h
 * @author Jackey Cen
 * @brief Plans and writes a Prefab or a scene's unsaved Prefab dependencies in order.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabFormat.h"
#include "prefab/PrefabRegistry.h"
#include "scene/SceneAsset.h"

#include <filesystem>
#include <functional>
#include <vector>

namespace mir
{
    struct PrefabSaveStep
    {
        PrefabId mPrefab{};
        std::filesystem::path mPath;
    };

    struct PrefabSavePlan
    {
        std::vector<PrefabSaveStep> mSteps; // dependencies first
        std::vector<PrefabDiagnostic> mDiagnostics;
    };

    struct PrefabSaveResult
    {
        bool mSucceeded = false;
        std::vector<PrefabId> mWritten;
        std::vector<PrefabDiagnostic> mDiagnostics;
    };

    // True when the prefab has changes the disk file does not hold, or has no file yet.
    using PrefabUnsavedPredicate = std::function<bool(PrefabId)>;

    // The prefab itself, then its unsaved dependencies.
    PrefabSavePlan PlanSavePrefab(const PrefabRegistry &registry, PrefabId id,
        const PrefabUnsavedPredicate &unsaved);

    // Unsaved prefabs the scene's instances reference, and their unsaved dependencies.
    PrefabSavePlan PlanSaveScene(const PrefabRegistry &registry, const LoadedScene &scene,
        const PrefabUnsavedPredicate &unsaved);

    // Writes in plan order and records each success in the registry. A failure
    // writes nothing more; already written files stay written and listed.
    PrefabSaveResult ExecuteSavePlan(PrefabRegistry &registry, const PrefabSavePlan &plan);
}
