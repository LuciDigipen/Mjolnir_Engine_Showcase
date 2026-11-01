/**
 * @file IMeshResolveHost.h
 * @author sumin.park
 * @brief The renderer calls MeshResolveSystem makes, so its entity logic runs without a GPU.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/IMeshUploader.h>
#include <renderer/Material.h>
#include <renderer/Mesh.h>
#include <renderer/MeshLoadTracker.h>
#include <renderer/Texture.h>

#include <algorithm>
#include <cstdint>
#include <span>

namespace mir
{
    // The synchronous creation and release a resolve system needs beside the asynchronous uploads.
    class IMeshResolveHost : public IMeshUploader
    {
    public:
        // One GPU mesh cache per catalog revision, shared across Worlds using this renderer.
        std::shared_ptr<MeshLoadTracker> MeshLoads(AsyncAssetService *assets)
        {
            const uint32_t revision = assets ? assets->Revision() : 0;
            std::erase_if(mMeshCaches, [assets, revision](const MeshCache &cache)
                {
                    if (cache.assets != assets || cache.revision == revision || cache.loads.use_count() != 1)
                        return false;
                    cache.loads->EvictUnused();
                    return cache.loads->Empty();
                });
            for (const auto &cache : mMeshCaches)
                if (cache.assets == assets && cache.revision == revision)
                    return cache.loads;
            auto loads = std::make_shared<MeshLoadTracker>(*this, assets);
            mMeshCaches.push_back({ assets, revision, loads });
            return loads;
        }

        void ReleaseCachedMeshes()
        {
            for (auto &cache : mMeshCaches)
            {
                std::vector<MeshHandle> ready;
                cache.loads->Release(ready);
                for (MeshHandle handle : ready)
                    RetireMesh(handle);
            }
            mMeshCaches.clear();
        }

        // Builtin shapes are small, so they are created in place.
        virtual MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices) = 0;
        virtual MaterialHandle CreateMaterial(const MaterialDesc &desc) = 0;
        virtual MaterialHandle ErrorMaterial() const = 0;
        // Releases the listed handles only, the others stay valid.
        virtual void ReleaseModelResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials, std::span<const TextureHandle> textures) = 0;

    private:
        struct MeshCache
        {
            AsyncAssetService *assets;
            uint32_t revision;
            std::shared_ptr<MeshLoadTracker> loads;
        };
        std::vector<MeshCache> mMeshCaches;
    };
}
