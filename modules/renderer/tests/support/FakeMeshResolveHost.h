/**
 * @file FakeMeshResolveHost.h
 * @author sumin.park
 * @brief A resolve host wrapping FakeMeshUploader, so MeshResolveSystem runs without a GPU.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/IMeshResolveHost.h>

#include "support/FakeMeshUploader.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace mir::test
{
    // The uploads go to uploader, which the test steps. Synchronous meshes get generation 2 so they never read
    // as an upload, and every released handle is recorded.
    class FakeMeshResolveHost final : public IMeshResolveHost
    {
    public:
        static constexpr uint32_t kSyncGeneration = 2;

        MeshHandle QueueMesh(MeshUploadSource source, const MeshUploadOptions &options) override
        {
            return uploader.QueueMesh(std::move(source), options);
        }
        MeshUploadState MeshState(MeshHandle handle) const override { return uploader.MeshState(handle); }
        bool RetireMesh(MeshHandle handle) override { return uploader.RetireMesh(handle); }
        uint64_t MeshFootprint(uint32_t vertexCount, uint32_t indexCount, const MeshUploadOptions &options) const override
        {
            return uploader.MeshFootprint(vertexCount, indexCount, options);
        }
        MeshStreamingStats MeshStreaming() const override { return uploader.MeshStreaming(); }
        uint64_t UploadFrameBudget() const override { return uploader.UploadFrameBudget(); }

        MeshHandle CreateMesh(std::span<const Vertex> vertices, std::span<const uint32_t>) override
        {
            if (vertices.empty())
                return MeshHandle{};
            ++syncMeshes;
            return MeshHandle{ mNextSync++, kSyncGeneration };
        }

        MaterialHandle CreateMaterial(const MaterialDesc &desc) override
        {
            createdMaterials.push_back(desc);
            return MaterialHandle{ mNextMaterial++, 1 };
        }
        MaterialHandle ErrorMaterial() const override { return MaterialHandle{ 9999, 1 }; }

        void ReleaseModelResources(std::span<const MeshHandle> meshes, std::span<const MaterialHandle> materials,
            std::span<const TextureHandle>) override
        {
            for (const MeshHandle mesh : meshes)
            {
                releasedMeshes.push_back(mesh);
                if (mesh.mGeneration != kSyncGeneration)
                    uploader.RetireMesh(mesh);
            }
            releasedMaterials.insert(releasedMaterials.end(), materials.begin(), materials.end());
        }

        FakeMeshUploader uploader;
        std::size_t syncMeshes = 0;
        std::vector<MeshHandle> releasedMeshes;
        std::vector<MaterialHandle> releasedMaterials;
        std::vector<MaterialDesc> createdMaterials;

    private:
        uint32_t mNextSync = 0;
        uint32_t mNextMaterial = 0;
    };
}
