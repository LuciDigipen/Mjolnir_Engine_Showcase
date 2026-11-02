/**
 * @file MeshResolveSystem.h
 * @author Sumin Park
 * @brief Checks on each MeshRenderer's GPU handles and MeshBounds
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ComponentFieldChanges.h>
#include <core/ecs/System.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/SurfaceMaterialLibrary.h>
#include <renderer/components/MeshBounds.h>

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir
{
    class AssetCache;
    class AssetResolver;
    class VulkanRenderer;

    // Resolves resources every frame
    class MeshResolveSystem final : public ISystem
    {
    public:
        // materials is the shared surface library a registered mesh surface is
        // borrowed from; null keeps every material owned by this system.
        MeshResolveSystem(VulkanRenderer &renderer, SurfaceMaterialLibrary *materials, AssetCache *assets,
            const AssetResolver &resolver);

        // Call at ReleaseSceneResources
        void Reset();

        // MeshRenderer's OnRemove: a packed id the World hands out again starts unresolved
        void Forget(Entity entity) { mResolved.erase(PackEntity(entity)); }

        // Frees this system's uploads while other Worlds keep rendering.
        // Borrowed surface handles stay with the library.
        void ReleaseResources();
        void CommitReload(AssetCache *assets);

        // The library outlives every other owner; stop reaching it first.
        void DetachSurfaceMaterials() { mSurfaceMaterials = nullptr; mUseOwner = 0; }

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnStop(SystemContext &context) override;

    private:
        // a renamed mesh or shader resolves again; a removal is Forget's
        static void OnBeforeFieldChange(World &world, std::span<const ComponentFieldChange> changes, void *user);

        struct CachedMesh
        {
            MeshHandle mMesh;
            std::optional<MeshBounds> mBounds;
        };

        struct CachedMaterial
        {
            MaterialHandle mMaterial;
            // A shared surface handle the library owns and may replace.
            bool mBorrowed = false;
            std::string mSurfacePath;
            AssetId mSurfaceAsset;
        };

        // the names an entity's handles were last resolved from
        struct ResolvedNames
        {
            std::string mMesh;
            std::string mShader;
            // Set while this renderer draws a shared library surface.
            bool mSurface = false;
            std::string mSurfacePath;
            AssetId mSurfaceAsset;
        };

        const CachedMesh &ResolveMesh(const std::string &meshName);
        MaterialHandle ResolveMaterial(const std::string &shaderName, ResolvedNames &resolved);
        const SurfaceMaterialRegistration *FindMeshSurface(std::string_view shaderName) const;
        void PublishUses();
        void RebindSurfaceMaterials();

        VulkanRenderer &mRenderer;
        SurfaceMaterialLibrary *mSurfaceMaterials = nullptr;
        AssetCache *mAssets;
        const AssetResolver &mResolver;
        uint64_t mUseOwner = 0;
        uint64_t mLibraryGeneration = 0;

        std::unordered_map<std::string, CachedMesh> mMeshes;          // by meshName
        std::unordered_map<std::string, CachedMaterial> mMaterials;   // by materialShader
        std::unordered_map<uint64_t, ResolvedNames> mResolved;        // by PackEntity
        std::map<std::string, SurfaceMaterialUse> mUses;              // by logicalPath
    };
}
