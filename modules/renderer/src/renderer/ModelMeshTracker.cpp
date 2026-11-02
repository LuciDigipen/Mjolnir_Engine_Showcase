/**
 * @file ModelMeshTracker.cpp
 * @author sumin.park
 * @brief Queued uploads of in-memory model primitives on the shared upload lifecycle.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/ModelMeshTracker.h"

#include <functional>

namespace mir
{
    size_t GpuRecordKeyHash::operator()(const GpuRecordKey &key) const
    {
        size_t hash = std::hash<uint64_t>{}(key.asset.value);
        const auto mix = [&hash](uint64_t value)
            { hash ^= std::hash<uint64_t>{}(value) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2); };
        mix(key.part);
        mix(static_cast<uint64_t>(key.content.kind));
        mix(key.content.value);
        mix(key.options.rayTracing ? 1 : 0);
        return hash;
    }

    ModelMeshTracker::ModelMeshTracker(IMeshUploader &uploader, AsyncAssetService *assets)
        : Base(assets, kMeshAssetDecoder,
            [&uploader](std::shared_ptr<const DecodedMesh> mesh)
                { return uploader.QueueMesh({ mesh->vertices, mesh->indices, mesh }, { .rayTracing = true }); },
            [&uploader](MeshHandle handle) { return uploader.MeshState(handle); },
            [&uploader](MeshHandle handle) { uploader.RetireMesh(handle); },
            [&uploader](MeshHandle, const DecodedMesh &mesh)
                { return uploader.MeshFootprint(static_cast<uint32_t>(mesh.vertices.size()),
                    static_cast<uint32_t>(mesh.indices.size()), { .rayTracing = true }); },
            [](MeshLoad &view, const DecodedMesh &mesh) { view.bounds = mesh.bounds; },
            [&uploader] { return uploader.MeshStreaming().retiringMeshBytes; }, uploader.SharedResidency(), GpuCategory::Mesh)
    {}

    AssetId ModelMeshTracker::Request(const GpuRecordKey &key, const Make &make, const std::string &label)
    {
        const AssetId id{ mIds.try_emplace(key, mIds.size() + 1).first->second };
        RequestInline(id, make, label);
        return id;
    }
}
