/**
 * @file ModelMeshTracker.h
 * @author sumin.park
 * @brief Shared GPU mesh records for primitives that arrive as memory, keyed by content and creation options.
 *
 * A model's primitives come from a typed mesh asset, not the raw mesh format MeshLoadTracker decodes, so the
 * caller supplies the vertices and this tracker only owns the queued upload, the claims and the residency.
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
    // What a record's bytes came from. Until blob content identity exists the catalog revision stands in, so
    // a changed asset never reuses an old record. Equal values mean equal content.
    struct ContentIdentity
    {
        enum class Kind : uint8_t { Revision, Content };
        Kind kind = Kind::Revision;
        uint64_t value = 0;

        static constexpr ContentIdentity ForRevision(uint32_t revision) { return { Kind::Revision, revision }; }
        static constexpr ContentIdentity ForContent(uint64_t hash) { return { Kind::Content, hash }; }
        constexpr bool operator==(const ContentIdentity &) const = default;
    };

    // The complete identity of one GPU mesh record.
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

        // Queues the record's upload unless it exists, make runs only then. The result names the record for a
        // ModelMeshLease.
        AssetId Request(const GpuRecordKey &key, const Make &make, const std::string &label);

    private:
        // Collision-free, an id is the next number in the order keys first appear.
        std::unordered_map<GpuRecordKey, uint64_t, GpuRecordKeyHash> mIds;
    };
    using ModelMeshLease = AssetUploadLease<ModelMeshTracker>;
}
