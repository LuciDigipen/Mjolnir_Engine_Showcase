/**
 * @file ModelMeshTracker.h
 * @author sumin.park
 * @brief Caches GPU mesh primitives by file identity and upload options.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/MeshLoadTracker.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace mir
{
    // Use the file hash when known, otherwise the reload number prevents stale reuse.
    struct ContentIdentity
    {
        enum class Kind : uint8_t { Revision, Content };
        Kind kind = Kind::Revision;
        uint64_t value = 0;

        static constexpr ContentIdentity ForRevision(uint32_t revision) { return { Kind::Revision, revision }; }
        static constexpr ContentIdentity ForContent(uint64_t hash) { return { Kind::Content, hash }; }
        constexpr bool operator==(const ContentIdentity &) const = default;
    };

    struct GpuRecordKey
    {
        // The cooked asset the record was made from, and the primitive within it.
        AssetId asset;
        uint32_t part = 0;
        ContentIdentity content;
        MeshUploadOptions options;

        bool operator==(const GpuRecordKey &other) const
        {
            return asset == other.asset && part == other.part && content == other.content &&
                options.rayTracing == other.options.rayTracing;
        }
    };

    struct GpuRecordKeyHash
    {
        size_t operator()(const GpuRecordKey &key) const;
    };

    class ModelMeshTracker : public AssetUploadCache<DecodedMesh, MeshHandle, MeshLoad, &MeshLoad::mesh>
    {
        using Base = AssetUploadCache<DecodedMesh, MeshHandle, MeshLoad, &MeshLoad::mesh>;
    public:
        using Make = std::function<std::shared_ptr<const DecodedMesh>()>;

        ModelMeshTracker(IMeshUploader &uploader, AsyncAssetService *assets);

        // make runs only for a missing entry. The returned ID is for a ModelMeshLease.
        AssetId Request(const GpuRecordKey &key, const Make &make, const std::string &label);

    private:
        std::unordered_map<GpuRecordKey, uint64_t, GpuRecordKeyHash> mIds;
        // The options the record was keyed with, read when its upload is queued.
        std::unordered_map<uint64_t, MeshUploadOptions> mOptions;
    };
    using ModelMeshLease = AssetUploadLease<ModelMeshTracker>;
}
