/**
 * @file PrefabDocuments.cpp
 * @author Jackey Cen
 * @brief The open Prefab documents, each with an isolated preview World, and their saves.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "PrefabDocuments.h"

#include "MeshResolveSystem.h"
#include "PrefabAuthoring.h"

#include <animation/ModelPlayer.h>
#include <animation/ModelSystem.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <editor/Selection.h>
#include <prefab/PrefabSave.h>
#include <prefab/PrefabSceneInstance.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/ViewDrawSource.h>
#include <scene/SceneAsset.h>

#include <utility>
#include <vector>

namespace mir
{
    struct PrefabDocuments::Document
    {
        World mWorld;
        LoadedScene mScene;
        CommandBuffer mCommands;
        SystemScheduler mScheduler;
        MeshResolveSystem *mMesh = nullptr;
        ModelSystem *mModel = nullptr;
    };

    PrefabDocuments::PrefabDocuments(PrefabCoordinator &coordinator, PrefabAuthoringAdapter &authoring)
        : mCoordinator(coordinator), mAuthoring(authoring)
    {
    }

    PrefabDocuments::~PrefabDocuments()
    {
        mActive.reset();
        mAuthoring.SetActiveDocument(std::nullopt);
        for (const auto &[id, document] : mDocuments)
        {
            (void)document;
            mCoordinator.UnbindPreview(id);
        }
        mDocuments.clear();
    }

    void PrefabDocuments::BindResolveSystems(VulkanRenderer *renderer, AssetCache *assets, std::string assetIdRoot)
    {
        mRenderer = renderer;
        mAssets = assets;
        mAssetIdRoot = std::move(assetIdRoot);
    }

    bool PrefabDocuments::Open(PrefabId id)
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
            const std::vector<PrefabDiagnostic> diagnostics = mCoordinator.Cache().Compile(*document);
            if (HasPrefabErrors(diagnostics))
                return false;
        }

        const std::shared_ptr<const CompiledPrefab> prefab = mCoordinator.Cache().Find(id);
        if (!prefab)
            return false;

        auto preview = std::make_unique<Document>();
        World &world = preview->mWorld;
        InstallHierarchy(world);
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

        preview->mScheduler.AddSystem<TransformPropagateSystem>(SystemPhase::PostUpdate);
        if (mRenderer != nullptr)
        {
            preview->mMesh = &preview->mScheduler.AddSystem<MeshResolveSystem>(SystemPhase::PostUpdate, *mRenderer,
                mAssets, mAssetIdRoot);
            preview->mModel = &preview->mScheduler.AddSystem<ModelSystem>(SystemPhase::PostUpdate, *mRenderer,
                mAssets, mAssetIdRoot);
        }

        SystemContext context{ world, preview->mCommands };
        preview->mScheduler.Start(context);

        mDocuments.emplace(id, std::move(preview));
        mActive = id;
        mAuthoring.SetActiveDocument(id);
        return true;
    }

    bool PrefabDocuments::Close(PrefabId id)
    {
        const auto open = mDocuments.find(id);
        if (open == mDocuments.end())
            return false;

        mCoordinator.UnbindPreview(id);

        Document &document = *open->second;
        if (document.mMesh != nullptr)
            document.mMesh->Reset();
        if (document.mModel != nullptr)
            document.mModel->Reset();
        document.mWorld.Resource<ModelDrawItems>().items.clear();
        document.mWorld.Resource<ModelMorphOverrides>().instances.clear();

        mDocuments.erase(open);
        if (mActive == id)
        {
            mActive.reset();
            mAuthoring.SetActiveDocument(std::nullopt);
        }
        return true;
    }

    bool PrefabDocuments::IsOpen(PrefabId id) const
    {
        return mDocuments.contains(id);
    }

    World *PrefabDocuments::PreviewWorld(PrefabId id) const
    {
        const auto open = mDocuments.find(id);
        return open == mDocuments.end() ? nullptr : &open->second->mWorld;
    }

    const LoadedScene *PrefabDocuments::PreviewScene(PrefabId id) const
    {
        const auto open = mDocuments.find(id);
        return open == mDocuments.end() ? nullptr : &open->second->mScene;
    }

    void PrefabDocuments::Update(float dt, double elapsed, uint64_t frame)
    {
        for (auto &[id, document] : mDocuments)
        {
            (void)id;
            SystemContext context{ document->mWorld, document->mCommands, dt, elapsed, frame };
            document->mScheduler.Update(context, 1, dt);
        }
    }

    void PrefabDocuments::ResetResolve()
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

    bool PrefabDocuments::SavePrefab(PrefabId id, const PrefabUnsaved &unsaved, const PrefabSaved &markSaved)
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

    bool PrefabDocuments::SaveScene(const LoadedScene &scene, const PrefabUnsaved &unsaved,
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
