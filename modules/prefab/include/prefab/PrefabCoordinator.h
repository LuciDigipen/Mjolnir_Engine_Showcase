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
        bool SetDocument(const PrefabDocument &doc, const std::filesystem::path &intendedPath,
            std::vector<PrefabDiagnostic> *diagnostics = nullptr);
        bool RemoveDocument(PrefabId id);

        // Rebuilds the disk table and logs what Scan found.
        void ScanSources(const std::filesystem::path &prefabsDir);

        // Applies the same scan and queues every changed source for the next
        // Commit. While frozen the scan waits for the next unfrozen Commit.
        void RescanSources(const std::filesystem::path &prefabsDir);

        // Effective documents replaced by external source changes, once per adopted id.
        std::vector<PrefabId> ConsumeExternalChanges();

        // Frozen pins the published prefab state: Commit publishes nothing and
        // rescans wait for the next unfrozen Commit.
        void SetFrozen(bool frozen) { mFrozen = frozen; }
        bool IsFrozen() const { return mFrozen; }

        std::vector<PrefabId> DocumentIds() const { return mRegistry.Ids(); }

        // Non-owning: the scene must outlive this binding.
        void Bind(LoadedScene &scene);
        LoadedScene *Scene() { return mScene; }
        const LoadedScene *Scene() const { return mScene; }

        // A preview World and its scene live outside the main scene and are
        // committed by Commit in their own World.
        bool BindPreview(PrefabId id, World &world, LoadedScene &scene);
        bool UnbindPreview(PrefabId id);
        World *PreviewWorld(PrefabId id) const;
        LoadedScene *PreviewScene(PrefabId id) const;

        PrefabSceneResolver &Resolver() { return mResolver; }
        const PrefabSceneResolver &Resolver() const { return mResolver; }

        // Compiles pending documents and propagates every newer template to the
        // open scene's instances and every bound preview.
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

        using PublishedTable = std::map<StableId, PublishedInstance>;

        struct PreviewBinding
        {
            World *mWorld = nullptr;
            LoadedScene *mScene = nullptr;
            PublishedTable mPublished;
        };

        void RecordPublished(const LoadedScene &scene, PublishedTable &published) const;
        void ReconcilePublished(const LoadedScene &scene, PublishedTable &published);
        void CommitScene(World &world, LoadedScene &scene, PublishedTable &published, bool mainScene);
        bool SceneHasPendingWork(const LoadedScene *scene, const PublishedTable &published) const;
        void ApplyRescan(const std::filesystem::path &prefabsDir);
        void AddPendingDocument(PrefabId id);
        std::vector<PrefabId> PendingInDependencyOrder() const;

        PrefabRegistry mRegistry;
        CompiledPrefabCache mCache;
        PrefabSceneResolver mResolver;
        LoadedScene *mScene = nullptr;
        std::set<PrefabId> mPendingDocuments;
        std::set<PrefabId> mExternalChanges;
        PublishedTable mPublished;
        std::map<PrefabId, PreviewBinding> mPreviews;
        std::filesystem::path mQueuedRescan;
        bool mFrozen = false;
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
