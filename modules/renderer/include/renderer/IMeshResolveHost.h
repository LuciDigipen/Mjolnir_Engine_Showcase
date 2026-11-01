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
#include <renderer/MaterialRecordCache.h>
#include <renderer/Mesh.h>
#include <renderer/MeshLoadTracker.h>
#include <renderer/ModelMeshTracker.h>
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
        // One GPU mesh cache shared across Worlds and catalog revisions, requests carry the decoded content so
        // a changed asset never reuses an old record.
        std::shared_ptr<MeshLoadTracker> MeshLoads(AsyncAssetService *assets)
        {
            return mMeshCaches.Get(*this, assets, true);
        }

        // One GPU record cache for primitives that arrive as memory, keyed by content like MeshLoads.
        std::shared_ptr<ModelMeshTracker> ModelMeshes(AsyncAssetService *assets)
        {
            return mModelMeshCaches.Get(*this, assets, true);
        }

        // Materials shared by every World using this renderer, keyed by their resolved description.
        std::shared_ptr<MaterialRecordCache> MaterialRecords()
        {
            if (!mMaterialRecords)
                mMaterialRecords = std::make_shared<MaterialRecordCache>(*this);
            return mMaterialRecords;
        }

        void ReleaseCachedMeshes()
        {
            mMeshCaches.Release([this](MeshHandle handle) { RetireMesh(handle); });
            mModelMeshCaches.Release([this](MeshHandle handle) { RetireMesh(handle); });
        }

        // Forgets the records, the renderer destroys the materials themselves.
        void ReleaseCachedMaterials()
        {
            if (mMaterialRecords)
                mMaterialRecords->Reset();
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
        AssetUploadRegistry<ModelMeshTracker> mModelMeshCaches;
        std::shared_ptr<MaterialRecordCache> mMaterialRecords;
    };
}
