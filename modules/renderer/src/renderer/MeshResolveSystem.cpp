/**
 * @file MeshResolveSystem.cpp
 * @author Sumin Park
 * @brief Checks on each MeshRenderer's GPU handles and MeshBounds
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshResolveSystem.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetId.h>
#include <assets/AssetResolver.h>
#include <core/ecs/Query.h>
#include <core/ecs/TypeId.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <renderer/Shapes.h>
#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/VulkanRenderer.h>
#include <renderer/components/MeshRenderer.h>

#include <serial/JsonFile.h>

#include <glm/common.hpp>

#include <algorithm>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        // MeshData from a mesh JSON asset
        std::optional<MeshData> ParseMeshAsset(std::string_view text, std::string_view assetPath)
        {
            const std::optional<nlohmann::json> json = ParseJson(text, assetPath);
            if (!json)
                return std::nullopt;

            MeshData mesh;
            try
            {
                for (const auto &v : json->value("vertices", nlohmann::json::array()))
                {
                    Vertex vertex{};
                    const auto &p = v.at("position");
                    const auto &c = v.at("color");
                    const auto &n = v.at("normal");
                    vertex.pos = glm::vec3(p.at(0).get<float>(), p.at(1).get<float>(), p.at(2).get<float>());
                    vertex.color = glm::vec3(c.at(0).get<float>(), c.at(1).get<float>(), c.at(2).get<float>());
                    vertex.normal = glm::vec3(n.at(0).get<float>(), n.at(1).get<float>(), n.at(2).get<float>());
                    mesh.vertices.push_back(vertex);
                }
                mesh.indices = json->value("indices", std::vector<uint32_t>{});
            }
            catch (const nlohmann::json::exception &exception)
            {
                MIR_LOG_ERROR("MeshResolveSystem: '{}' is not a valid mesh: {}", assetPath, exception.what());
                return std::nullopt;
            }

            // an index past the vertices would read out of bounds on the GPU
            for (const uint32_t index : mesh.indices)
            {
                if (index >= mesh.vertices.size())
                {
                    MIR_LOG_ERROR("MeshResolveSystem: '{}' has index {} past its {} vertices", assetPath, index,
                        mesh.vertices.size());
                    return std::nullopt;
                }
            }
            return mesh;
        }

        MeshBounds ComputeMeshBounds(std::span<const Vertex> vertices)
        {
            glm::vec3 min(std::numeric_limits<float>::max());
            glm::vec3 max(std::numeric_limits<float>::lowest());

            for (const Vertex &vertex : vertices)
            {
                min = glm::min(min, vertex.pos);
                max = glm::max(max, vertex.pos);
            }

            if (vertices.empty())
                min = max = glm::vec3(0.0f);

            return MeshBounds{ min, max };
        }
    }

    MeshResolveSystem::MeshResolveSystem(VulkanRenderer &renderer, SurfaceMaterialLibrary *materials,
        AssetCache *assets, const AssetResolver &resolver)
        : mRenderer(renderer), mSurfaceMaterials(materials), mAssets(assets), mResolver(resolver)
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
        // A borrowed surface handle belongs to the library, which may still draw it.
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

    void MeshResolveSystem::CommitReload(AssetCache *assets)
    {
        std::vector<MeshHandle> meshes;
        for (const auto &[name, cached] : mMeshes)
        {
            if (!cached.mMesh.IsNull())
                meshes.push_back(cached.mMesh);
        }
        mRenderer.ReleaseModelResources(meshes, {}, {});

        // materials come from shaders, not the asset cache, so they stay
        mAssets = assets;
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

        // Primitives (Shapes.h) generate their own geometry under a reserved name - no asset file, so no asset-cache lookup.
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

        const std::string name = mResolver.PathOf(id);
        const AssetBlobView *blob = mAssets != nullptr ? mAssets->Load(id) : nullptr;
        if (blob == nullptr)
        {
            MIR_LOG_ERROR("MeshResolveSystem: could not load '{}'", name);
            return cached;
        }

        if (const std::optional<MeshData> mesh = ParseMeshAsset(AsStringView(*blob), name))
        {
            cached.mMesh = mRenderer.CreateMesh(mesh->vertices, mesh->indices);
            cached.mBounds = ComputeMeshBounds(mesh->vertices);
        }
        return cached;
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

        const SurfaceMaterialRegistration *const surface = FindMeshSurface(shaderName);
        if (mSurfaceMaterials != nullptr && surface != nullptr)
        {
            const std::string logicalPath = surface->asset;
            const AssetId asset = mResolver.ResolveFull(logicalPath);
            std::string error;
            const std::optional<SurfaceMaterialBinding> binding =
                mSurfaceMaterials->Acquire(logicalPath, asset, "mesh", &error);
            if (binding)
            {
                resolved.mSurface = true;
                resolved.mSurfacePath = logicalPath;
                resolved.mSurfaceAsset = asset;
                return mMaterials
                    .emplace(shaderName, CachedMaterial{ .mMaterial = binding->material, .mBorrowed = true,
                                                        .mSurfacePath = logicalPath, .mSurfaceAsset = asset })
                    .first->second.mMaterial;
            }
            // Retried when the library generation moves; an error material
            // would hide that the authored surface failed.
            MIR_LOG_ERROR("MeshResolveSystem: surface material '{}': {}", shaderName, error);
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

    const SurfaceMaterialRegistration *MeshResolveSystem::FindMeshSurface(std::string_view shaderName) const
    {
        for (const SurfaceMaterialRegistration &registration : mRenderer.SurfaceMaterials().materials)
            if (registration.frontend == "mesh" && registration.stem == shaderName)
                return &registration;
        return nullptr;
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
                                                                     .frontend = "mesh",
                                                                     .required = true });
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
        const uint64_t generation = mSurfaceMaterials->Generation();
        if (generation == mLibraryGeneration)
            return;
        mLibraryGeneration = generation;

        // The library replaced or migrated the shared handles, so every borrowed
        // cache entry and resolved shader name is stale.
        std::erase_if(mMaterials, [](const auto &entry) { return entry.second.mBorrowed; });
        for (auto &[entity, names] : mResolved)
            names.mShader.clear();
    }

    void MeshResolveSystem::OnUpdate(SystemContext &context)
    {
        World &world = context.world;

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

        world.GetOrCreateQuery<MeshRenderer>().ForEach(
            [&](Entity entity, MeshRenderer &meshRenderer)
            {
                ResolvedNames &resolved = mResolved[PackEntity(entity)];
                const std::string_view meshName = meshRenderer.meshName;
                const std::string_view shaderName = meshRenderer.materialShader;

                if (resolved.mMesh != meshName)
                {
                    resolved.mMesh = meshName;
                    const CachedMesh *cached = meshName.empty() ? nullptr : &ResolveMesh(resolved.mMesh);
                    meshRenderer.mesh = cached != nullptr ? cached->mMesh : MeshHandle{};
                    pendingBounds.emplace_back(entity, cached != nullptr ? cached->mBounds : std::nullopt);
                }

                if (resolved.mShader != shaderName)
                {
                    resolved.mShader = shaderName;
                    resolved.mSurface = false;
                    resolved.mSurfacePath.clear();
                    resolved.mSurfaceAsset = kNullAssetId;
                    meshRenderer.material = shaderName.empty()
                        ? MaterialHandle{}
                        : ResolveMaterial(resolved.mShader, resolved);
                }
            });

        PublishUses();

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
