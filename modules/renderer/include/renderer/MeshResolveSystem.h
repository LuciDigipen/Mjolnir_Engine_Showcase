/**
 * @file MeshResolveSystem.h
 * @author Sumin Park
 * @brief Checks on each MeshRenderer's GPU handles and MeshBounds
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/EntityChanges.h>
#include <core/ecs/System.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/components/MeshBounds.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir
{
    class AssetCache;
    class VulkanRenderer;

    // Resolves resources every frame
    class MeshResolveSystem final : public ISystem
    {
    public:
        MeshResolveSystem(VulkanRenderer &renderer, AssetCache *assets, std::string assetIdRoot);

        // Call at ReleaseSceneResources
        void Reset();

        void CommitReload(AssetCache *assets);

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnStop(SystemContext &context) override;

    private:
        static void OnBeforeRemove(World &world, const EntityChangeSet &changes, void *user);

        struct CachedMesh
        {
            MeshHandle mMesh;
            std::optional<MeshBounds> mBounds;
        };

        // the names an entity's handles were last resolved from
        struct ResolvedNames
        {
            std::string mMesh;
            std::string mShader;
        };

        const CachedMesh &ResolveMesh(const std::string &meshName);
        MaterialHandle ResolveMaterial(const std::string &shaderName);

        VulkanRenderer &mRenderer;
        AssetCache *mAssets;
        std::string mAssetIdRoot;

        std::unordered_map<std::string, CachedMesh> mMeshes;         // by meshName
        std::unordered_map<std::string, MaterialHandle> mMaterials;  // by materialShader
        std::unordered_map<uint64_t, ResolvedNames> mResolved;       // by PackEntity
    };
}
