/**
 * @file MeshResolveSystem.cpp
 * @author Sumin Park
 * @brief Checks on each MeshRenderer's GPU handles and MeshBounds
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshResolveSystem.h>

#include <assets/AssetId.h>
#include <assets/AssetPath.h>
#include <assets/AssetResolver.h>
#include <core/ecs/Query.h>
#include <core/ecs/SceneLoadGroup.h>
#include <core/ecs/TypeId.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <renderer/MeshAssetDecoder.h>
#include <renderer/Shapes.h>
#include <renderer/IMeshResolveHost.h>
#include <renderer/components/MeshRenderer.h>

#include <algorithm>
#include <span>
#include <utility>
#include <unordered_set>
#include <vector>

namespace mir
{
    MeshResolveSystem::MeshResolveSystem(IMeshResolveHost &renderer, AsyncAssetService *assets,
        const AssetResolver &resolver, SurfaceMaterialLibrary *materials)
        : mRenderer(renderer), mResolver(resolver), mAssets(assets), mTracker(renderer.MeshLoads(assets)),
          mSurfaceMaterials(materials)
    {
        if (mSurfaceMaterials != nullptr)
        {
            mUseOwner = mSurfaceMaterials->AddMaterialUseOwner();
            mLibraryGeneration = mSurfaceMaterials->Generation();
        }
    }

    void MeshResolveSystem::Reset()
    {
        mMeshes.clear();
        mMaterials.clear();
        mResolved.clear();
        mUses.clear();
        if (mSurfaceMaterials != nullptr && mUseOwner != 0)
            mSurfaceMaterials->SetMaterialUses(mUseOwner, {});
    }

    void MeshResolveSystem::ReleaseResources()
    {
        std::vector<MeshHandle> meshes;
        std::vector<MaterialHandle> materials;
        for (const auto &[name, cached] : mMeshes)
            if (!cached.mMesh.IsNull())
                meshes.push_back(cached.mMesh);
        // Asset meshes belong to the shared cache, this World owns only its builtin meshes and materials.
        for (const auto &[name, cached] : mMaterials)
            if (!cached.mBorrowed && !cached.mMaterial.IsNull())
                materials.push_back(cached.mMaterial);
        if (!meshes.empty() || !materials.empty())
            mRenderer.ReleaseModelResources(meshes, materials, {});
        Reset();
        if (mSurfaceMaterials != nullptr && mUseOwner != 0)
        {
            mSurfaceMaterials->RemoveMaterialUseOwner(mUseOwner);
            mUseOwner = 0;
        }
    }

    void MeshResolveSystem::CommitReload()
    {
        std::vector<MeshHandle> meshes;
        for (const auto &[name, cached] : mMeshes)
        {
            if (!cached.mMesh.IsNull())
                meshes.push_back(cached.mMesh);
        }
        mRenderer.ReleaseModelResources(meshes, {}, {});

        // materials come from shaders, not the asset cache, so they stay
        mMeshes.clear();
        mTracker = mRenderer.MeshLoads(mAssets);
        mResolved.clear();
    }

    void MeshResolveSystem::OnStart(SystemContext &context)
    {
        if (ComponentFieldListeners *listeners = context.world.TryResource<ComponentFieldListeners>())
            listeners->AddBeforeChange(&MeshResolveSystem::OnBeforeFieldChange, this);
    }

    void MeshResolveSystem::OnStop(SystemContext &context)
    {
        if (ComponentFieldListeners *listeners = context.world.TryResource<ComponentFieldListeners>())
            listeners->RemoveBeforeChange(&MeshResolveSystem::OnBeforeFieldChange, this);
    }

    void MeshResolveSystem::OnBeforeFieldChange(World &, std::span<const ComponentFieldChange> changes, void *user)
    {
        auto *self = static_cast<MeshResolveSystem *>(user);
        const TypeId meshType = TypeIdOf<MeshRenderer>();

        for (const ComponentFieldChange &change : changes)
        {
            if (!(change.mType == meshType))
                continue;

            for (const std::string &field : change.mFields)
            {
                if (field != "meshName" && field != "materialShader")
                    continue;
                self->mResolved.erase(PackEntity(change.mEntity));
                break;
            }
        }
    }

    const MeshResolveSystem::CachedMesh &MeshResolveSystem::ResolveMesh(const std::string &meshName)
    {
        if (const auto it = mMeshes.find(meshName); it != mMeshes.end())
            return it->second;

        CachedMesh &cached = mMeshes[meshName];

        // Primitives (Shapes.h) generate their own geometry under a reserved name - no asset file, so no load.
        if (const std::optional<MeshData> builtin = MakeBuiltinMesh(meshName))
        {
            cached.mMesh = mRenderer.CreateMesh(builtin->vertices, builtin->indices);
            cached.mBounds = ComputeMeshBounds(builtin->vertices);
            return cached;
        }

        std::string reason;
        const AssetId id = mResolver.Resolve(meshName, &reason);
        if (id.IsNull())
        {
            MIR_LOG_ERROR("MeshResolveSystem: mesh '{}': {}", meshName, reason);
            return cached;
        }

        cached.mAsset = id;
        const UploadKey key{ id, UploadContent(mAssets, id) };
        mTracker->Request(key, mResolver.PathOf(id));
        cached.mLease = MeshLoadLease(mTracker, key);
        return cached;
    }

    MeshResolveSystem::MeshView MeshResolveSystem::ViewOf(const CachedMesh &cached) const
    {
        if (cached.mAsset.IsNull())
            return MeshView{ cached.mMesh, cached.mBounds };

        const MeshLoad *load = cached.mLease.Get();
        if (load == nullptr)
            return MeshView{};
        // the draw path skips a mesh that is not Ready, so the queued handle goes on at once
        return MeshView{ load->mesh,
            load->phase == MeshLoadPhase::Ready ? std::optional<MeshBounds>(load->bounds) : std::nullopt };
    }

    bool MeshResolveSystem::Failed(const CachedMesh &cached)
    {
        if (cached.mAsset.IsNull())
            return true;
        const MeshLoad *load = cached.mLease.Get();
        return load == nullptr || load->phase == MeshLoadPhase::Failed;
    }

    SurfaceMaterialLookup MeshResolveSystem::LookupSurfaceMaterial(std::string_view reference) const
    {
        if (mSurfaceMaterials == nullptr)
            return SurfaceMaterialLookup{};
        if (!reference.ends_with(".material.json"))
            return mSurfaceMaterials->LookupMeshSurface(reference);

        std::string error;
        AssetId id = mResolver.Resolve(reference, &error);
        if (id.IsNull())
        {
            const AssetPathResult normalized = NormalizeAssetPath(reference);
            if (normalized.Ok())
                id = mResolver.ResolveFull(normalized.path);
        }
        if (id.IsNull())
        {
            SurfaceMaterialLookup lookup;
            lookup.status = SurfaceMaterialLookupStatus::Missing;
            lookup.error = "explicit material '" + std::string(reference) + "' is not a cooked asset";
            if (!error.empty())
                lookup.error += ": " + error;
            return lookup;
        }
        SurfaceMaterialLookup lookup = mSurfaceMaterials->LookupMeshSurface(mResolver.PathOf(id));
        if (lookup.status == SurfaceMaterialLookupStatus::NotSurface)
        {
            lookup.status = SurfaceMaterialLookupStatus::Missing;
            lookup.error = "the explicit material '" + std::string(reference) +
                "' declares no mesh surface registration";
        }
        return lookup;
    }

    MaterialHandle MeshResolveSystem::ResolveMaterial(const std::string &shaderName, ResolvedNames &resolved)
    {
        resolved.mSurface = false;
        resolved.mSurfacePath.clear();
        resolved.mSurfaceAsset = kNullAssetId;

        if (const auto it = mMaterials.find(shaderName); it != mMaterials.end())
        {
            resolved.mSurface = it->second.mBorrowed;
            resolved.mSurfacePath = it->second.mSurfacePath;
            resolved.mSurfaceAsset = it->second.mSurfaceAsset;
            return it->second.mMaterial;
        }

        const SurfaceMaterialLookup surface = LookupSurfaceMaterial(shaderName);
        if (surface.status == SurfaceMaterialLookupStatus::Found)
        {
            const std::string logicalPath = surface.registration->asset;
            const AssetId asset = mResolver.ResolveFull(logicalPath);
            const SurfaceMaterialResolution resolution = mSurfaceMaterials->Resolve(logicalPath, asset, "mesh");
            if (resolution.status == SurfaceMaterialStatus::Ready && resolution.binding)
            {
                resolved.mSurface = true;
                resolved.mSurfacePath = logicalPath;
                resolved.mSurfaceAsset = asset;
                return mMaterials
                    .emplace(shaderName, CachedMaterial{ .mMaterial = resolution.binding->material,
                                                        .mBorrowed = true,
                                                        .mSurfacePath = logicalPath,
                                                        .mSurfaceAsset = asset,
                                                        .mSurfaceStatus = SurfaceMaterialStatus::Ready })
                    .first->second.mMaterial;
            }
            if (resolution.status == SurfaceMaterialStatus::Pending)
            {
                // The first load draws the engine placeholder; publication moves the library generation and
                // resolves this shader again.
                resolved.mSurface = true;
                resolved.mSurfacePath = logicalPath;
                resolved.mSurfaceAsset = asset;
                return mMaterials
                    .emplace(shaderName, CachedMaterial{ .mMaterial = mRenderer.ErrorMaterial(),
                                                        .mBorrowed = true,
                                                        .mSurfacePath = logicalPath,
                                                        .mSurfaceAsset = asset,
                                                        .mSurfaceStatus = SurfaceMaterialStatus::Pending })
                    .first->second.mMaterial;
            }
            // Retried when the library generation moves; an error material
            // would hide that the authored surface failed.
            MIR_LOG_ERROR("MeshResolveSystem: surface material '{}': {}", shaderName, resolution.error);
            return mRenderer.ErrorMaterial();
        }
        if (surface.status == SurfaceMaterialLookupStatus::Ambiguous ||
            surface.status == SurfaceMaterialLookupStatus::Missing)
        {
            MIR_LOG_ERROR("MeshResolveSystem: material '{}': {}", shaderName, surface.error);
            return mRenderer.ErrorMaterial();
        }

        // Null would draw the default grey, which hides that the authored shader failed.
        MaterialHandle material = mRenderer.CreateMaterial(MaterialDesc{ .shaderName = shaderName });
        if (material.IsNull())
        {
            MIR_LOG_ERROR("MeshResolveSystem: material '{}' failed, using error material", shaderName);
            material = mRenderer.ErrorMaterial();
        }
        return mMaterials.emplace(shaderName, CachedMaterial{ .mMaterial = material }).first->second.mMaterial;
    }

    void MeshResolveSystem::PublishUses()
    {
        if (mSurfaceMaterials == nullptr || mUseOwner == 0)
            return;

        // Only assets a live renderer still draws: a deleted entity or a
        // reassigned materialShader must not keep an asset required for cooks.
        std::map<std::string, SurfaceMaterialUse> live;
        for (const auto &[packed, names] : mResolved)
            if (names.mSurface)
                live.emplace(names.mSurfacePath, SurfaceMaterialUse{ .logicalPath = names.mSurfacePath,
                                                                     .asset = names.mSurfaceAsset,
                                                                     .frontend = "mesh" });
        if (live == mUses)
            return;
        mUses = std::move(live);

        std::vector<SurfaceMaterialUse> uses;
        uses.reserve(mUses.size());
        for (const auto &[logicalPath, use] : mUses)
            uses.push_back(use);
        mSurfaceMaterials->SetMaterialUses(mUseOwner, uses);
    }

    void MeshResolveSystem::RebindSurfaceMaterials()
    {
        if (mSurfaceMaterials == nullptr)
            return;
        // Scene unload releases this owner, while the resolver is reused by the next scene.
        if (mUseOwner == 0)
            mUseOwner = mSurfaceMaterials->AddMaterialUseOwner();
        const uint64_t generation = mSurfaceMaterials->Generation();
        if (generation == mLibraryGeneration)
            return;
        mLibraryGeneration = generation;

        // The library replaced or migrated the shared handles, so every borrowed
        // cache entry and resolved material is stale.
        std::erase_if(mMaterials, [](const auto &entry) { return entry.second.mBorrowed; });
        for (auto &[entity, names] : mResolved)
            names.mMaterialInvalidated = true;
    }

    void MeshResolveSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;

        if (mSurfaceMaterials != nullptr)
            mSurfaceMaterials->Advance();
        mTracker->Update();
        RebindSurfaceMaterials();

        std::vector<Entity> staleBounds;
        std::erase_if(mResolved,
            [&](const auto &entry)
            {
                const Entity entity = UnpackEntity(entry.first);
                if (world.IsAlive(entity) && world.Has<MeshRenderer>(entity))
                    return false;
                if (world.IsAlive(entity))
                    staleBounds.push_back(entity);
                return true;
            });

        std::vector<std::pair<Entity, std::optional<MeshBounds>>> pendingBounds;
        std::unordered_set<std::string> usedMeshes;
        std::unordered_set<std::string> usedShaders;

        uint32_t required = 0, settled = 0;
        world.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity entity, MeshRenderer &meshRenderer)
            {
                ResolvedNames &resolved = mResolved[PackEntity(entity)];
                const std::string_view meshName = meshRenderer.meshName;
                const std::string_view shaderName = meshRenderer.materialShader;
                usedMeshes.emplace(meshName);
                usedShaders.emplace(shaderName);

                if (resolved.mMesh != meshName)
                {
                    resolved.mMesh = meshName;
                    // a name changed while the old mesh loads leaves that load to the tracker, other entities may use it
                    const MeshView view = meshName.empty() ? MeshView{} : ViewOf(ResolveMesh(resolved.mMesh));
                    meshRenderer.mesh = view.mMesh;
                    pendingBounds.emplace_back(entity, view.mBounds);
                }
                else if (!meshName.empty() && !world.Has<MeshBounds>(entity))
                {
                    const auto cached = mMeshes.find(resolved.mMesh);
                    if (cached != mMeshes.end())
                    {
                        const MeshView view = ViewOf(cached->second);
                        meshRenderer.mesh = view.mMesh;
                        pendingBounds.emplace_back(entity, view.mBounds);
                    }
                }

                if (resolved.mShader != shaderName || resolved.mMaterialInvalidated)
                {
                    resolved.mShader = shaderName;
                    resolved.mMaterialInvalidated = false;
                    resolved.mSurface = false;
                    resolved.mSurfacePath.clear();
                    resolved.mSurfaceAsset = kNullAssetId;
                    meshRenderer.material = shaderName.empty() ? MaterialHandle{} : ResolveMaterial(resolved.mShader, resolved);
                }

                if (!meshName.empty())
                {
                    ++required;
                    const auto cached = mMeshes.find(resolved.mMesh);
                    const bool meshSettled =
                        world.Has<MeshBounds>(entity) || (cached != mMeshes.end() && Failed(cached->second));
                    bool materialPending = false;
                    if (resolved.mSurface)
                    {
                        const auto material = mMaterials.find(resolved.mShader);
                        materialPending = material != mMaterials.end() &&
                            material->second.mSurfaceStatus == SurfaceMaterialStatus::Pending;
                    }
                    if (meshSettled && !materialPending)
                        ++settled;
                }
            });
        PublishUses();

        if (SceneLoadGroup *group = world.TryResource<SceneLoadGroup>())
            group->Report(this, required, settled);

        std::vector<MeshHandle> releasedMeshes;
        std::vector<MaterialHandle> releasedMaterials;
        std::erase_if(mMeshes, [&](const auto &item)
            {
                if (usedMeshes.contains(item.first))
                    return false;
                if (item.second.mAsset.IsNull() && !item.second.mMesh.IsNull())
                    releasedMeshes.push_back(item.second.mMesh);
                return true;
            });
        std::erase_if(mMaterials, [&](const auto &item)
            {
                if (usedShaders.contains(item.first))
                    return false;
                if (!item.second.mBorrowed && !item.second.mMaterial.IsNull())
                    releasedMaterials.push_back(item.second.mMaterial);
                return true;
            });
        if (!releasedMeshes.empty() || !releasedMaterials.empty())
            mRenderer.ReleaseModelResources(releasedMeshes, releasedMaterials, {});

        for (const Entity entity : staleBounds)
        {
            if (world.Has<MeshBounds>(entity))
                world.RemoveComponent<MeshBounds>(entity);
        }

        for (const auto &[entity, bounds] : pendingBounds)
        {
            if (bounds)
            {
                if (MeshBounds *existing = world.GetComponent<MeshBounds>(entity))
                    *existing = *bounds;
                else
                    world.AddComponent<MeshBounds>(entity, *bounds);
            }
            else if (world.Has<MeshBounds>(entity))
            {
                world.RemoveComponent<MeshBounds>(entity);
            }
        }
        if (mSurfaceMaterials != nullptr && mUseOwner != 0)
            mSurfaceMaterials->AcknowledgeGeneration(mUseOwner, mLibraryGeneration);
    }
}
