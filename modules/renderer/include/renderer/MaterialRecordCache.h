/**
 * @file MaterialRecordCache.h
 * @author sumin.park
 * @brief Shared material records keyed by the complete resolved description, holding their texture claims.
 *
 * Two descriptions that compare equal by value share one GPU material. A record keeps the texture claims
 * that made its handles valid, so a texture outlives every material that samples it. Unused records stay
 * until the renderer is over its residency target, because they pin textures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/Material.h>
#include <renderer/TextureLoadTracker.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace mir
{
    class IMeshResolveHost;

    // A material description compared and hashed by value, owning the bytes a MaterialDesc only views.
    struct MaterialValueKey
    {
        std::string shaderName;
        VkCullModeFlags cullMode = 0;
        VkPolygonMode polygonMode = VK_POLYGON_MODE_FILL;
        VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkFrontFace frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        TextureHandle baseColorTexture;
        TextureHandle normalTexture;
        SamplerDesc sampler;
        MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
        float alphaCutoff = 0.5f;
        std::vector<std::byte> record;
        uint32_t recordAlignment = 16;
        bool surfaceMaterial = false;
        std::string surfaceAsset;

        static MaterialValueKey Of(const MaterialDesc &desc);
        bool operator==(const MaterialValueKey &) const;
    };

    struct MaterialValueKeyHash
    {
        size_t operator()(const MaterialValueKey &key) const;
    };

    class MaterialRecordCache
    {
    public:
        explicit MaterialRecordCache(IMeshResolveHost &host);
        ~MaterialRecordCache();
        MaterialRecordCache(const MaterialRecordCache &) = delete;
        MaterialRecordCache &operator=(const MaterialRecordCache &) = delete;

        // The record's id, or zero when the renderer could not create the material. The claims move into a new
        // record and are dropped when the record exists, they must cover every texture the description names.
        uint64_t Acquire(const MaterialDesc &desc, std::vector<TextureLoadLease> claims);
        void Retain(uint64_t id);
        void Drop(uint64_t id);
        MaterialHandle Find(uint64_t id) const;

        // Releases unused records while the renderer is over its residency target, and only those whose release
        // helps: the ones that hold the last claim on a texture, or the record bytes of an over-full category.
        void Trim();
        // Releases every unused record. Acquire does it before giving up on a full material table.
        void EvictUnused();
        // The GPU bytes the records hold in the shared material table, and the texture bytes that only unused
        // records keep alive, which is what Trim can give back.
        uint64_t RecordBytes() const;
        uint64_t PinnedTextureBytes() const;
        // Forgets every record without releasing handles, the renderer destroyed them.
        void Reset();
        size_t Size() const { return mRecords.size(); }

    private:
        struct Record
        {
            MaterialValueKey key;
            MaterialHandle handle;
            std::vector<TextureLoadLease> claims;
            size_t consumers = 0;
            uint64_t lastUse = 0;
            uint64_t recordBytes = 0;
        };
        void Evict(std::unordered_map<uint64_t, Record>::iterator record);

        IMeshResolveHost &mHost;
        std::shared_ptr<GpuResidency> mResidency;
        uint32_t mHolder = 0;
        std::unordered_map<uint64_t, Record> mRecords;
        std::unordered_map<MaterialValueKey, uint64_t, MaterialValueKeyHash> mIds;
        uint64_t mNextId = 1;
        uint64_t mUseSerial = 0;
    };

    // One consumer claim on a material record.
    class MaterialLease
    {
    public:
        MaterialLease() = default;
        MaterialLease(std::shared_ptr<MaterialRecordCache> cache, uint64_t id) : mCache(std::move(cache)), mId(id)
        {
            if (mCache && mId != 0)
                mCache->Retain(mId);
        }
        ~MaterialLease() { Reset(); }
        MaterialLease(const MaterialLease &) = delete;
        MaterialLease &operator=(const MaterialLease &) = delete;
        MaterialLease(MaterialLease &&other) noexcept : mCache(std::move(other.mCache)), mId(other.mId)
        {
            other.mId = 0;
        }
        MaterialLease &operator=(MaterialLease &&other) noexcept;
        void Reset();
        // Null for a default lease or after the renderer released every material.
        MaterialHandle Get() const { return mCache && mId != 0 ? mCache->Find(mId) : MaterialHandle{}; }

    private:
        std::shared_ptr<MaterialRecordCache> mCache;
        uint64_t mId = 0;
    };
}
