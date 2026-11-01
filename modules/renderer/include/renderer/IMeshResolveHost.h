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
            return mMeshCaches.Get(*this, assets);
        }

        void ReleaseCachedMeshes()
        {
            mMeshCaches.Release([this](MeshHandle handle) { RetireMesh(handle); });
        }

        // Builtin shapes are small, so they are created in place.
        virtual MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t> indices) = 0;
        virtual MaterialHandle CreateMaterial(const MaterialDesc &desc) = 0;
        virtual MaterialHandle ErrorMaterial() const = 0;
        // Releases the listed handles only, the others stay valid.
        virtual void ReleaseModelResources(std::span<const MeshHandle> meshes,
            std::span<const MaterialHandle> materials, std::span<const TextureHandle> textures) = 0;

    private:
        AssetUploadRegistry<MeshLoadTracker> mMeshCaches;
    };
}
