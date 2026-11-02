/**
 * @file PrefabWorkspace.h
 * @author Jackey Cen
 * @brief The open Prefab documents, each with an isolated preview World, and their saves.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <prefab/PrefabCoordinator.h>
#include <prefab/PrefabId.h>

#include <core/ecs/Entity.h>
#include <core/math/WorldCoordinates.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace mir
{
    class AssetCache;
    class AssetResolver;
    class AsyncAssetService;
    struct LoadedScene;
    class PrefabAuthoringAdapter;
    class ModelLibrary;
    class VulkanRenderer;
    class World;

    class PrefabWorkspace
    {
    public:
        using PrefabUnsaved = std::function<bool(PrefabId)>;
        using PrefabSaved = std::function<void(PrefabId)>;

        PrefabWorkspace(PrefabCoordinator &coordinator, PrefabAuthoringAdapter &authoring);
        ~PrefabWorkspace();

        PrefabWorkspace(const PrefabWorkspace &) = delete;
        PrefabWorkspace &operator=(const PrefabWorkspace &) = delete;
        PrefabWorkspace(PrefabWorkspace &&) = delete;
        PrefabWorkspace &operator=(PrefabWorkspace &&) = delete;

        // Null renderer skips preview mesh and model systems; null assets still permits built-in meshes.
        // models is the App's library of prepared models, the previews take their models from it with the scene.
        void BindResolveSystems(VulkanRenderer *renderer, AsyncAssetService *assetService,
            const AssetResolver &resolver, std::shared_ptr<ModelLibrary> models);

        // Focuses an open document, or creates its preview World and one instance.
        bool Open(PrefabId id);
        bool Close(PrefabId id);
        bool IsOpen(PrefabId id) const;

        // The Scene tab: no document is active, so edits author the bound scene.
        void ActivateScene();

        // Open documents in the order their tabs were opened.
        std::vector<PrefabId> OpenDocuments() const;

        // The file stem, or a short UUID before the document has a file.
        std::string LabelOf(PrefabId id) const;

        std::optional<PrefabId> FindByPath(const std::filesystem::path &path) const;

        // Places a linked instance at position, local to parent or in the world for a root.
        Entity Place(World &world, PrefabId id, Entity parent, const WorldVec3 &position);

        // Closes the tabs of documents Undo removed from the registry.
        void CloseMissingDocuments();

        std::optional<PrefabId> ActiveDocument() const { return mActive; }
        World *PreviewWorld(PrefabId id) const;
        const LoadedScene *PreviewScene(PrefabId id) const;

        // Runs each preview's transform, mesh-resolve and model systems once.
        void Update(float dt, double elapsed, uint64_t frame);

        // Starts background replacement of every open preview's models, the old ones keep drawing.
        void CommitReload();

        // Invalidate History save points without clearing undo/redo or marking a disk write.
        void InvalidateExternalSavePoints(const std::function<void(PrefabId)> &invalidate);

        // @p unsaved also has to report a prefab with no file yet; @p markSaved
        // runs once per file that was written.
        bool SavePrefab(PrefabId id, const PrefabUnsaved &unsaved, const PrefabSaved &markSaved);

        // Writes the unsaved prefabs the scene uses, then calls @p writeScene.
        bool SaveScene(const LoadedScene &scene, const PrefabUnsaved &unsaved, const PrefabSaved &markSaved,
            const std::function<bool()> &writeScene);

    private:
        struct Document;

        PrefabCoordinator &mCoordinator;
        PrefabAuthoringAdapter &mAuthoring;
        std::map<PrefabId, std::unique_ptr<Document>> mDocuments;
        // Tab order: the order the documents were opened
        std::vector<PrefabId> mOpenOrder;
        std::optional<PrefabId> mActive;
        VulkanRenderer *mRenderer = nullptr;
        AsyncAssetService *mAssetService = nullptr;
        std::shared_ptr<ModelLibrary> mModels;
        const AssetResolver *mResolver = nullptr;
    };
}
