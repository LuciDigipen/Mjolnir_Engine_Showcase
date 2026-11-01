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
#include <assets/AssetResolver.h>
#include <core/ecs/Query.h>
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
#include <vector>

namespace mir
{
    MeshResolveSystem::MeshResolveSystem(IMeshResolveHost &renderer, AsyncAssetService *assets,
        const AssetResolver &resolver)
        : mRenderer(renderer), mResolver(resolver), mTracker(renderer, assets)
    {}

    void MeshResolveSystem::Reset()
    {
        mTracker.Reset();
        mMeshes.clear();
        mMaterials.clear();
        mResolved.clear();
    }

    void MeshResolveSystem::ReleaseResources()
    {
        std::vector<MeshHandle> meshes;
        std::vector<MaterialHandle> materials;
        for (const auto &[name, cached] : mMeshes)
            if (!cached.mMesh.IsNull())
                meshes.push_back(cached.mMesh);
        // queued and uploading meshes are retired, only the ready ones are released here
        mTracker.Release(meshes);
        for (const auto &[name, material] : mMaterials)
            if (!material.IsNull())
                materials.push_back(material);
        if (!meshes.empty() || !materials.empty())
            mRenderer.ReleaseModelResources(meshes, materials, {});
        Reset();
    }

    void MeshResolveSystem::CommitReload()
    {
        std::vector<MeshHandle> meshes;
        for (const auto &[name, cached] : mMeshes)
        {
            if (!cached.mMesh.IsNull())
                meshes.push_back(cached.mMesh);
        }
        mTracker.Release(meshes);
        mRenderer.ReleaseModelResources(meshes, {}, {});

        // materials come from shaders, not the asset cache, so they stay
        mMeshes.clear();
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

        // the tracker reports a failed load once, and this entry remembers it
        cached.mAsset = id;
        mTracker.Request(id, mResolver.PathOf(id));
        return cached;
    }

    MeshResolveSystem::MeshView MeshResolveSystem::ViewOf(const CachedMesh &cached) const
    {
        if (cached.mAsset.IsNull())
            return MeshView{ cached.mMesh, cached.mBounds };

        const MeshLoad *load = mTracker.Find(cached.mAsset);
        if (load == nullptr)
            return MeshView{};
        // the draw path skips a mesh that is not Ready, so the queued handle goes on at once
        return MeshView{ load->mesh,
            load->phase == MeshLoadPhase::Ready ? std::optional<MeshBounds>(load->bounds) : std::nullopt };
    }

    MaterialHandle MeshResolveSystem::ResolveMaterial(const std::string &shaderName)
    {
        if (const auto it = mMaterials.find(shaderName); it != mMaterials.end())
            return it->second;

        // Null would draw the default grey, which hides that the authored shader failed.
        MaterialHandle material = mRenderer.CreateMaterial(MaterialDesc{ .shaderName = shaderName });
        if (material.IsNull())
        {
            MIR_LOG_ERROR("MeshResolveSystem: material '{}' failed, using error material", shaderName);
            material = mRenderer.ErrorMaterial();
        }
        return mMaterials.emplace(shaderName, material).first->second;
    }

    void MeshResolveSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;

        // the assets whose load moved a step since last frame
        const std::vector<AssetId> advanced = mTracker.Update();

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

        world.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity entity, MeshRenderer &meshRenderer)
            {
                ResolvedNames &resolved = mResolved[PackEntity(entity)];
                const std::string_view meshName = meshRenderer.meshName;
                const std::string_view shaderName = meshRenderer.materialShader;

                if (resolved.mMesh != meshName)
                {
                    resolved.mMesh = meshName;
                    // a name changed while the old mesh loads leaves that load to the tracker, other entities may use it
                    const MeshView view = meshName.empty() ? MeshView{} : ViewOf(ResolveMesh(resolved.mMesh));
                    meshRenderer.mesh = view.mMesh;
                    pendingBounds.emplace_back(entity, view.mBounds);
                }
                else if (!advanced.empty() && !meshName.empty())
                {
                    // the entity's mesh loaded a step, take its handle and bounds
                    const auto cached = mMeshes.find(resolved.mMesh);
                    if (cached != mMeshes.end() && std::ranges::find(advanced, cached->second.mAsset) != advanced.end())
                    {
                        const MeshView view = ViewOf(cached->second);
                        meshRenderer.mesh = view.mMesh;
                        pendingBounds.emplace_back(entity, view.mBounds);
                    }
                }

                if (resolved.mShader != shaderName)
                {
                    resolved.mShader = shaderName;
                    meshRenderer.material = shaderName.empty() ? MaterialHandle{} : ResolveMaterial(resolved.mShader);
                }
            });

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
    }
}
