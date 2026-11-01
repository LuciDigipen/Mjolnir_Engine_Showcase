/**
 * @file IMeshUploader.h
 * @author sumin.park
 * @brief The renderer's asynchronous mesh upload interface, so callers and tests need no GPU.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/GpuResidency.h>
#include <renderer/Mesh.h>
#include <renderer/MeshStreaming.h>

#include <cstdint>
#include <memory>

namespace mir
{
    // Main thread only. A queued mesh is copied through the staging ring within the per-frame byte budget,
    // is Ready once that frame completes, and a retired one is freed once every frame that may use it completes.
    class IMeshUploader
    {
    public:
        virtual ~IMeshUploader() = default;

        // Null when the mesh cannot be queued, and the source's keepalive is dropped once the data is copied
        virtual MeshHandle QueueMesh(MeshUploadSource source, const MeshUploadOptions &options) = 0;
        virtual MeshUploadState MeshState(MeshHandle handle) const = 0;
        // The handle goes stale at once, false when it already was.
        virtual bool RetireMesh(MeshHandle handle) = 0;
        // GPU bytes a queued mesh of this size holds, BLAS included.
        virtual uint64_t MeshFootprint(uint32_t vertexCount, uint32_t indexCount,
            const MeshUploadOptions &options) const = 0;
        virtual MeshStreamingStats MeshStreaming() const = 0;
        // The GPU bytes one frame may write, shared by every consumer.
        virtual uint64_t UploadFrameBudget() const = 0;
        // The residency target the uploader's caches share, one per renderer. Caches keep it alive, a lease
        // may outlive the renderer.
        virtual std::shared_ptr<GpuResidency> SharedResidency() { return mResidency; }
        GpuResidency &Residency() { return *SharedResidency(); }

    private:
        std::shared_ptr<GpuResidency> mResidency = std::make_shared<GpuResidency>();
    };
}
