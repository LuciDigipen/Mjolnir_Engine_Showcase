/**
 * @file FakeMeshUploader.h
 * @author sumin.park
 * @brief A mesh uploader whose frames the test steps, so upload users run without a GPU.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/IMeshUploader.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace mir::test
{
    // A queued mesh stays Pending until the test records, completes or fails it, as a frame would.
    class FakeMeshUploader : public IMeshUploader
    {
    public:
        struct Mesh
        {
            MeshUploadState state = MeshUploadState::Pending;
            std::size_t vertices = 0;
            std::size_t indices = 0;
            MeshUploadOptions options;
            // dropped once recorded or retired, as the renderer drops it after the staging copy
            std::shared_ptr<const void> keepalive;
            // Nonzero once a test shrinks the mesh, as BLAS compaction does.
            uint64_t compactedBytes = 0;
        };

        MeshHandle QueueMesh(MeshUploadSource source, const MeshUploadOptions &options) override
        {
            if (rejectQueue || source.vertices.empty() || source.indices.empty())
                return MeshHandle{};
            ++queued;
            const uint32_t index = mNext++;
            meshes[index] = Mesh{ MeshUploadState::Pending, source.vertices.size(), source.indices.size(), options,
                std::move(source.keepalive) };
            return MeshHandle{ index, 1 };
        }

        MeshUploadState MeshState(MeshHandle handle) const override
        {
            const auto found = meshes.find(handle.mIndex);
            return found == meshes.end() || handle.mGeneration != 1 ? MeshUploadState::Stale : found->second.state;
        }

        bool RetireMesh(MeshHandle handle) override
        {
            const auto found = meshes.find(handle.mIndex);
            if (found == meshes.end() || handle.mGeneration != 1)
                return false;
            retired.push_back(handle);
            // a mesh the GPU may have used stays occupied until its last frame completes
            if (found->second.state != MeshUploadState::Pending)
                deferred.push_back(MeshBytes(handle));
            meshes.erase(found);
            return true;
        }

        uint64_t MeshFootprint(uint32_t vertexCount, uint32_t indexCount, const MeshUploadOptions &) const override
        {
            return uint64_t(vertexCount) * 64 + uint64_t(indexCount) * 4;
        }

        uint64_t MeshBytes(MeshHandle handle) const override
        {
            const auto found = meshes.find(handle.mIndex);
            if (found == meshes.end() || handle.mGeneration != 1)
                return 0;
            if (found->second.compactedBytes != 0)
                return found->second.compactedBytes;
            return MeshFootprint(uint32_t(found->second.vertices), uint32_t(found->second.indices),
                found->second.options);
        }

        MeshStreamingStats MeshStreaming() const override
        {
            MeshStreamingStats stats;
            for (const auto &[index, mesh] : meshes)
            {
                const uint64_t bytes = MeshFootprint(uint32_t(mesh.vertices), uint32_t(mesh.indices), mesh.options);
                if (mesh.state == MeshUploadState::Pending)
                {
                    ++stats.pending;
                    stats.pendingBytes += bytes;
                }
                else if (mesh.state == MeshUploadState::Uploading)
                {
                    ++stats.uploading;
                    stats.uploadingBytes += bytes;
                }
            }
            uint64_t deferredBytes = 0;
            for (const uint64_t bytes : deferred)
                deferredBytes += bytes;
            stats.retiring = retiringCount + deferred.size();
            stats.retiringBytes = retiringBytes + deferredBytes;
            stats.retiringMeshBytes = retiringBytes + deferredBytes;
            return stats;
        }

        uint64_t UploadFrameBudget() const override { return frameBudget; }

        // A frame records the mesh: its data is copied, so the keepalive goes.
        void Record(MeshHandle handle)
        {
            Mesh &mesh = meshes.at(handle.mIndex);
            mesh.state = MeshUploadState::Uploading;
            mesh.keepalive.reset();
        }

        void Complete(MeshHandle handle) { meshes.at(handle.mIndex).state = MeshUploadState::Ready; }

        void Fail(MeshHandle handle)
        {
            Mesh &mesh = meshes.at(handle.mIndex);
            mesh.state = MeshUploadState::Failed;
            mesh.keepalive.reset();
        }

        void CompleteFrame() { deferred.clear(); }

        // What a scene release does to every handle under the owner: they read Stale afterwards.
        void ReleaseAll() { meshes.clear(); }

        std::size_t Count(MeshUploadState state) const
        {
            std::size_t count = 0;
            for (const auto &[index, mesh] : meshes)
                count += mesh.state == state ? 1 : 0;
            return count;
        }

        std::map<uint32_t, Mesh> meshes;
        std::vector<MeshHandle> retired;
        // bytes of retired meshes still in use by a frame in flight
        std::vector<uint64_t> deferred;
        uint64_t queued = 0;
        // QueueMesh returns a null handle while set
        bool rejectQueue = false;
        uint64_t frameBudget = uint64_t(4) * 1024 * 1024;
        std::size_t retiringCount = 0;
        uint64_t retiringBytes = 0;

    private:
        uint32_t mNext = 0;
    };
}
