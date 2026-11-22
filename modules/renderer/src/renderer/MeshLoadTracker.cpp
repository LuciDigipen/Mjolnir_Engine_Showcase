/**
 * @file MeshLoadTracker.cpp
 * @author sumin.park
 * @brief Follows each mesh asset from its background decode to a usable GPU copy.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/MeshLoadTracker.h>

namespace mir
{
    MeshLoadTracker::MeshLoadTracker(IMeshUploader &uploader, AsyncAssetService *assets)
        : Base(assets, kMeshAssetDecoder,
            [&uploader](UploadKey, std::shared_ptr<const DecodedMesh> mesh)
            {
                return uploader.QueueMesh({ mesh->vertices, mesh->indices, mesh }, { .rayTracing = true });
            },
            [&uploader](MeshHandle handle) { return uploader.MeshState(handle); },
            [&uploader](MeshHandle handle) { uploader.RetireMesh(handle); },
            [&uploader](UploadKey, MeshHandle, const DecodedMesh &mesh)
            {
                return uploader.MeshFootprint(static_cast<uint32_t>(mesh.vertices.size()),
                    static_cast<uint32_t>(mesh.indices.size()), { .rayTracing = true });
            },
            [](MeshLoad &view, const DecodedMesh &mesh) { view.bounds = mesh.bounds; },
            [&uploader] { return uploader.MeshStreaming().retiringMeshBytes; }, uploader.SharedResidency(), GpuCategory::Mesh)
    {
        SetMeasure([&uploader](MeshHandle handle) { return uploader.MeshBytes(handle); });
    }
}
