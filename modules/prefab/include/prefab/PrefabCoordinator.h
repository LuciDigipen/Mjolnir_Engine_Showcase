/**
 * @file PrefabCoordinator.h
 * @author Jackey Cen
 * @brief Publishes compiled prefab edits to the open scene at the Edit safe point.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "prefab/CompiledPrefab.h"
#include "prefab/PrefabRegistry.h"
#include "prefab/PrefabSceneInstance.h"
#include "scene/SceneAsset.h"

#include <core/ecs/System.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

namespace mir
{
    class World;

    // Owns the prefab registry and compiled cache, and remembers the template
    // revision each open instance currently shows.
    class PrefabCoordinator
    {
    public:
        PrefabCoordinator();

        PrefabRegistry &Registry() { return mRegistry; }
        const PrefabRegistry &Registry() const { return mRegistry; }

        CompiledPrefabCache &Cache() { return mCache; }
        const CompiledPrefabCache &Cache() const { return mCache; }

        bool SetDocument(const PrefabDocument &doc, std::vector<PrefabDiagnostic> *diagnostics = nullptr);
        bool RemoveDocument(PrefabId id);

        // Rebuilds the disk table and logs what Scan found.
        void ScanSources(const std::filesystem::path &prefabsDir);

        // Non-owning: the scene must outlive this binding.
        void Bind(LoadedScene &scene);
        LoadedScene *Scene() { return mScene; }
        const LoadedScene *Scene() const { return mScene; }

        PrefabSceneResolver &Resolver() { return mResolver; }
        const PrefabSceneResolver &Resolver() const { return mResolver; }

        // Compiles pending documents and propagates every newer template to the
        // open scene's instances.
        void Commit(World &world);

        bool HasPendingWork() const;

        // True once after a Commit published instance data to the scene.
        bool ConsumePublishedChanges();

        // True once after a Commit changed the bound scene's authored data outside History:
        // references cleared, or instance bindings dropped or added.
        bool ConsumeSceneMaintenance();

        // Zero when no compiled template has been published to that instance.
        uint64_t PublishedRevision(StableId root) const;

    private:
        struct PublishedInstance
        {
            PrefabId mPrefab{};
            uint64_t mRevision = 0;
            std::shared_ptr<const CompiledPrefab> mTemplate;
        };

        void RecordPublished(const LoadedScene &scene);
        void ReconcilePublished(const LoadedScene &scene);

        PrefabRegistry mRegistry;
        CompiledPrefabCache mCache;
        PrefabSceneResolver mResolver;
        LoadedScene *mScene = nullptr;
        std::set<PrefabId> mPendingDocuments;
        std::map<StableId, PublishedInstance> mPublished;
        bool mPublishedChanges = false;
        bool mSceneMaintenance = false;
    };

    // Runs at PreUpdate, before any other system, and commits only while editing.
    class PrefabSystem final : public ISystem
    {
    public:
        void OnUpdate(SystemContext &context) override;
    };
}
