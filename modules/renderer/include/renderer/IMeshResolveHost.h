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
    class IMeshResolveHost : public IMeshUploader
    {
    public:
        std::shared_ptr<MeshLoadTracker> MeshLoads(AsyncAssetService *assets)
        {
            return mMeshCaches.Get(*this, assets, true);
        }

        std::shared_ptr<ModelMeshTracker> ModelMeshes(AsyncAssetService *assets)
        {
            return mModelMeshCaches.Get(*this, assets, true);
        }

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
        virtual bool MaterialSlotsExhausted() const { return false; }
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
