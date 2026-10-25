/**
 * @file PrefabWorkspace.cpp
 * @author Jackey Cen
 * @brief The open Prefab documents, each with an isolated preview World, and their saves.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/PrefabWorkspace.h>

#include <editor/PrefabAuthoringAdapter.h>

#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Transform.h>
#include <core/log/Log.h>
#include <editor/Selection.h>
#include <prefab/PrefabSave.h>
#include <prefab/PrefabSceneInstance.h>
#include <renderer/MeshResolveSystem.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/ViewDrawSource.h>
#include <scene/SceneAsset.h>
#include <scene/SceneIO.h>

#include <cstddef>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        constexpr std::size_t kShortIdLength = 8;
    }

    struct PrefabWorkspace::Document
    {
        World mWorld;
        LoadedScene mScene;
        CommandBuffer mCommands;
        SystemScheduler mScheduler;
        MeshResolveSystem *mMesh = nullptr;
        ModelSystem *mModel = nullptr;
    };

    PrefabWorkspace::PrefabWorkspace(PrefabCoordinator &coordinator, PrefabAuthoringAdapter &authoring)
        : mCoordinator(coordinator), mAuthoring(authoring)
    {
    }

    PrefabWorkspace::~PrefabWorkspace()
    {
        mActive.reset();
        mAuthoring.SetActiveDocument(std::nullopt);
        while (!mDocuments.empty())
            Close(mDocuments.begin()->first);
    }

    void PrefabWorkspace::BindResolveSystems(VulkanRenderer *renderer, AssetCache *assets, std::string assetIdRoot)
    {
        mRenderer = renderer;
        mAssets = assets;
        mAssetIdRoot = std::move(assetIdRoot);
    }

    bool PrefabWorkspace::Open(PrefabId id)
    {
        if (const auto open = mDocuments.find(id); open != mDocuments.end())
        {
            mActive = id;
            mAuthoring.SetActiveDocument(id);
            return true;
        }

        const PrefabDocument *document = mCoordinator.Registry().Find(id);
        if (document == nullptr)
            return false;

        if (!mCoordinator.Cache().Find(id))
        {
            const std::vector<PrefabDiagnostic> diagnostics =
                mCoordinator.Cache().Compile(*document, RegistryLookup(mCoordinator.Registry()));
            if (HasPrefabErrors(diagnostics))
                return false;
        }

        const std::shared_ptr<const CompiledPrefab> prefab = mCoordinator.Cache().Find(id);
        if (!prefab)
            return false;

        auto preview = std::make_unique<Document>();
        World &world = preview->mWorld;
        world.EmplaceResource<EngineModeState>();
        world.EmplaceResource<Selection>();
        world.EmplaceResource<EntityChangeListeners>();
        world.EmplaceResource<ModelDrawItems>();
        world.EmplaceResource<ModelMorphOverrides>();
        world.EmplaceResource<ViewDrawSources>();

        if (PlacePrefabInstance(world, preview->mScene, *prefab) == kNullStableId)
            return false;

        if (!mCoordinator.BindPreview(id, world, preview->mScene))
            return false;

        preview->mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate,
            SystemOrder{SystemOrder::kEarly});
        if (mRenderer != nullptr)
        {
            preview->mMesh = &preview->mScheduler.AddSystem<MeshResolveSystem>(SystemPhase::PostUpdate,
                SystemOrder{SystemOrder::kNormal}, *mRenderer,
                mAssets, mAssetIdRoot);
            preview->mModel = &preview->mScheduler.AddSystem<ModelSystem>(SystemPhase::PostUpdate,
                SystemOrder{SystemOrder::kNormal}, *mRenderer,
                mAssets, mAssetIdRoot);
        }

        SystemContext context{ world, preview->mCommands };
        preview->mScheduler.Start(context);

        mDocuments.emplace(id, std::move(preview));
        mOpenOrder.push_back(id);
        mActive = id;
        mAuthoring.SetActiveDocument(id);
        return true;
    }

    bool PrefabWorkspace::Close(PrefabId id)
    {
        const auto open = mDocuments.find(id);
        if (open == mDocuments.end())
            return false;

        mCoordinator.UnbindPreview(id);

        Document &document = *open->second;
        SystemContext context{ document.mWorld, document.mCommands };
        document.mScheduler.Stop(context);
        if (document.mMesh != nullptr)
            document.mMesh->ReleaseResources();
        if (document.mModel != nullptr)
            document.mModel->ReleaseResources();
        document.mWorld.Resource<ModelDrawItems>().items.clear();
        document.mWorld.Resource<ModelMorphOverrides>().instances.clear();

        mDocuments.erase(open);
        std::erase(mOpenOrder, id);
        if (mActive == id)
        {
            mActive.reset();
            mAuthoring.SetActiveDocument(std::nullopt);
        }
        return true;
    }

    bool PrefabWorkspace::IsOpen(PrefabId id) const
    {
        return mDocuments.contains(id);
    }

    void PrefabWorkspace::ActivateScene()
    {
        mActive.reset();
        mAuthoring.SetActiveDocument(std::nullopt);
    }

    std::vector<PrefabId> PrefabWorkspace::OpenDocuments() const
    {
        return mOpenOrder;
    }

    std::string PrefabWorkspace::LabelOf(PrefabId id) const
    {
        if (const std::optional<std::filesystem::path> path = mCoordinator.Registry().PathOf(id))
        {
            const std::u8string stem = path->stem().u8string();
            return std::string(stem.begin(), stem.end());
        }

        const std::string uuid = ToString(id);
        return uuid.substr(0, kShortIdLength);
    }

    std::optional<PrefabId> PrefabWorkspace::FindByPath(const std::filesystem::path &path) const
    {
        return mCoordinator.Registry().IdAtPath(path);
    }

    Entity PrefabWorkspace::Place(World &world, PrefabId id, Entity parent, const WorldVec3 &position)
    {
        const PrefabDocument *document = mCoordinator.Registry().Find(id);
        if (document == nullptr)
        {
            MIR_LOG_ERROR("Place Prefab: '{}' is not registered", ToString(id));
            return kNullEntity;
        }

        // A recompile would bump the revision and refresh every placed instance
        std::shared_ptr<const CompiledPrefab> prefab = mCoordinator.Cache().Find(id);
        if (!prefab)
        {
            const std::vector<PrefabDiagnostic> diagnostics =
                mCoordinator.Cache().Compile(*document, RegistryLookup(mCoordinator.Registry()));
            for (const PrefabDiagnostic &diagnostic : diagnostics)
                MIR_LOG_ERROR("Place Prefab: {}", diagnostic.mMessage);
            if (HasPrefabErrors(diagnostics))
                return kNullEntity;
            prefab = mCoordinator.Cache().Find(id);
        }
        if (!prefab)
            return kNullEntity;

        LoadedScene *scene = mCoordinator.Scene();
        if (scene == nullptr)
            return kNullEntity;

        const StableId rootId = PlacePrefabInstance(world, *scene, *prefab, parent);
        if (rootId == kNullStableId)
            return kNullEntity;

        const Entity root = scene->mEntities.at(rootId);
        if (Transform *transform = world.GetComponent<Transform>(root))
            transform->SetPosition(position);
        else
            AddTransform(world, root, Transform(position));

        const auto instance = scene->mInstances.find(rootId);
        if (instance != scene->mInstances.end())
            instance->second.mPlacement = InstancePlacementFromWorld(world, root);
        return root;
    }

    void PrefabWorkspace::CloseMissingDocuments()
    {
        std::vector<PrefabId> removed;
        for (const PrefabId id : mOpenOrder)
        {
            if (mCoordinator.Registry().Find(id) == nullptr)
                removed.push_back(id);
        }
        for (const PrefabId id : removed)
            Close(id);
    }

    World *PrefabWorkspace::PreviewWorld(PrefabId id) const
    {
        const auto open = mDocuments.find(id);
        return open == mDocuments.end() ? nullptr : &open->second->mWorld;
    }

    const LoadedScene *PrefabWorkspace::PreviewScene(PrefabId id) const
    {
        const auto open = mDocuments.find(id);
        return open == mDocuments.end() ? nullptr : &open->second->mScene;
    }

    void PrefabWorkspace::Update(float dt, double elapsed, uint64_t frame)
    {
        for (auto &[id, document] : mDocuments)
        {
            (void)id;
            SystemContext context{ document->mWorld, document->mCommands, dt, elapsed, frame };
            document->mScheduler.Update(context, 1, dt);
        }
    }

    bool PrefabWorkspace::PrepareReload(AssetCache &assets, std::string &error)
    {
        DiscardPreparedReload();
        for (const auto &[id, document] : mDocuments)
        {
            (void)id;
            if (document->mModel != nullptr && !document->mModel->PrepareReload(assets, error))
            {
                DiscardPreparedReload();
                return false;
            }
        }
        return true;
    }

    void PrefabWorkspace::CommitReload(AssetCache *assets)
    {
        mAssets = assets;
        for (const auto &[id, document] : mDocuments)
        {
            (void)id;
            if (document->mModel != nullptr)
                document->mModel->CommitReload(assets);
            if (document->mMesh != nullptr)
                document->mMesh->CommitReload(assets);
            document->mWorld.Resource<ModelDrawItems>().items.clear();
            document->mWorld.Resource<ModelMorphOverrides>().instances.clear();
        }
    }

    void PrefabWorkspace::DiscardPreparedReload()
    {
        for (const auto &[id, document] : mDocuments)
        {
            (void)id;
            if (document->mModel != nullptr)
                document->mModel->DiscardPreparedReload();
        }
    }

    void PrefabWorkspace::ResetResolve()
    {
        for (auto &[id, document] : mDocuments)
        {
            (void)id;
            if (document->mMesh != nullptr)
                document->mMesh->Reset();
            if (document->mModel != nullptr)
                document->mModel->Reset();
            document->mWorld.Resource<ModelDrawItems>().items.clear();
            document->mWorld.Resource<ModelMorphOverrides>().instances.clear();
        }
    }

    void PrefabWorkspace::InvalidateExternalSavePoints(const std::function<void(PrefabId)> &invalidate)
    {
        for (PrefabId id : mCoordinator.ConsumeExternalChanges())
            invalidate(id);
    }

    bool PrefabWorkspace::SavePrefab(PrefabId id, const PrefabUnsaved &unsaved, const PrefabSaved &markSaved)
    {
        const PrefabSavePlan plan = PlanSavePrefab(mCoordinator.Registry(), id, unsaved);
        const PrefabSaveResult result = ExecuteSavePlan(mCoordinator.Registry(), plan);
        for (PrefabId written : result.mWritten)
            markSaved(written);

        for (const PrefabDiagnostic &diagnostic : result.mDiagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error)
                MIR_LOG_ERROR("Save Prefab: {}", diagnostic.mMessage);
            else
                MIR_LOG_WARN("Save Prefab: {}", diagnostic.mMessage);
        }
        return result.mSucceeded;
    }

    bool PrefabWorkspace::SaveScene(const LoadedScene &scene, const PrefabUnsaved &unsaved,
        const PrefabSaved &markSaved, const std::function<bool()> &writeScene)
    {
        const PrefabSavePlan plan = PlanSaveScene(mCoordinator.Registry(), scene, unsaved);
        const PrefabSaveResult result = ExecuteSavePlan(mCoordinator.Registry(), plan);
        for (PrefabId written : result.mWritten)
            markSaved(written);

        for (const PrefabDiagnostic &diagnostic : result.mDiagnostics)
        {
            if (diagnostic.mSeverity == PrefabDiagnosticSeverity::Error)
                MIR_LOG_ERROR("Save Scene: {}", diagnostic.mMessage);
            else
                MIR_LOG_WARN("Save Scene: {}", diagnostic.mMessage);
        }

        return result.mSucceeded && writeScene();
    }
}
