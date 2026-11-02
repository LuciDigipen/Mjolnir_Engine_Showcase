/**
 * @file MeshResolveSystem.h
 * @author Sumin Park
 * @brief Checks on each MeshRenderer's GPU handles and MeshBounds
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <core/ecs/ComponentFieldChanges.h>
#include <core/ecs/System.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/MeshLoadTracker.h>
#include <renderer/components/MeshBounds.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace mir
{
    class AssetResolver;
    class AsyncAssetService;
    class IMeshResolveHost;

    // Resolves resources every frame. A mesh asset loads in the background, its entities draw it the frame its
    // upload completes and gain MeshBounds then. A builtin shape resolves in the same frame.
    class MeshResolveSystem final : public ISystem
    {
    public:
        // assets may be null, and every mesh asset then fails to load.
        MeshResolveSystem(IMeshResolveHost &renderer, AsyncAssetService *assets, const AssetResolver &resolver);

        // Call at ReleaseSceneResources
        void Reset();

        // MeshRenderer's OnRemove: a packed id the World hands out again starts unresolved
        void Forget(Entity entity) { mResolved.erase(PackEntity(entity)); }

        // Frees this system's uploads while other Worlds keep rendering.
        void ReleaseResources();
        void CommitReload();

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;
        void OnLoadingUpdate(SystemContext &context) override { OnUpdate(context); }
        void OnStop(SystemContext &context) override;

    private:
        // a renamed mesh or shader resolves again; a removal is Forget's
        static void OnBeforeFieldChange(World &world, std::span<const ComponentFieldChange> changes, void *user);

        struct CachedMesh
        {
            // builtin shapes only
            MeshHandle mMesh;
            std::optional<MeshBounds> mBounds;
            // non-null for a mesh asset
            AssetId mAsset;
            MeshLoadLease mLease;
        };

        // the names an entity's handles were last resolved from
        struct ResolvedNames
        {
            std::string mMesh;
            std::string mShader;
        };

        // The handle and bounds an entity of this mesh shows now. The handle is null until the upload is queued
        // and bounds exist only once it draws.
        struct MeshView
        {
            MeshHandle mMesh;
            std::optional<MeshBounds> mBounds;
        };

        const CachedMesh &ResolveMesh(const std::string &meshName);
        MeshView ViewOf(const CachedMesh &cached) const;
        // A failure is as final as a ready mesh, the scene stops waiting for it
        static bool Failed(const CachedMesh &cached);
        MaterialHandle ResolveMaterial(const std::string &shaderName);

        IMeshResolveHost &mRenderer;
        const AssetResolver &mResolver;
        AsyncAssetService *mAssets;
        std::shared_ptr<MeshLoadTracker> mTracker;

        std::unordered_map<std::string, CachedMesh> mMeshes;         // by meshName
        std::unordered_map<std::string, MaterialHandle> mMaterials;  // by materialShader
        std::unordered_map<uint64_t, ResolvedNames> mResolved;       // by PackEntity
    };
}
