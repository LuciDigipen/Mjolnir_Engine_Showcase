/**
 * @file MaterialRecordCache.cpp
 * @author sumin.park
 * @brief Value-keyed shared material records, texture claims and pressure eviction.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/MaterialRecordCache.h"

#include <renderer/IMeshResolveHost.h>

#include <algorithm>
#include <bit>
#include <functional>
#include <span>

namespace mir
{
    namespace
    {
        // Floats compare by bit pattern so a key and its hash agree.
        bool SameFloat(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }
    }

    MaterialValueKey MaterialValueKey::Of(const MaterialDesc &desc)
    {
        MaterialValueKey key;
        key.shaderName = desc.shaderName;
        key.cullMode = desc.cullMode;
        key.polygonMode = desc.polygonMode;
        key.topology = desc.topology;
        key.frontFace = desc.frontFace;
        key.baseColorTexture = desc.baseColorTexture;
        key.normalTexture = desc.normalTexture;
        key.sampler = desc.sampler;
        key.alphaMode = desc.alphaMode;
        key.alphaCutoff = desc.alphaCutoff;
        key.record.assign(desc.record.begin(), desc.record.end());
        key.recordAlignment = desc.recordAlignment;
        key.surfaceMaterial = desc.surfaceMaterial;
        key.surfaceAsset = desc.surfaceAsset;
        return key;
    }

    bool MaterialValueKey::operator==(const MaterialValueKey &other) const
    {
        return shaderName == other.shaderName && cullMode == other.cullMode && polygonMode == other.polygonMode &&
            topology == other.topology && frontFace == other.frontFace &&
            baseColorTexture == other.baseColorTexture && normalTexture == other.normalTexture &&
            sampler.magFilter == other.sampler.magFilter && sampler.minFilter == other.sampler.minFilter &&
            sampler.mipmapMode == other.sampler.mipmapMode && sampler.addressModeU == other.sampler.addressModeU &&
            sampler.addressModeV == other.sampler.addressModeV && sampler.useMips == other.sampler.useMips &&
            SameFloat(sampler.maxAnisotropy, other.sampler.maxAnisotropy) && alphaMode == other.alphaMode &&
            SameFloat(alphaCutoff, other.alphaCutoff) && record == other.record &&
            recordAlignment == other.recordAlignment && surfaceMaterial == other.surfaceMaterial &&
            surfaceAsset == other.surfaceAsset;
    }

    size_t MaterialValueKeyHash::operator()(const MaterialValueKey &key) const
    {
        size_t hash = std::hash<std::string>{}(key.shaderName);
        const auto mix = [&hash](uint64_t value)
            { hash ^= std::hash<uint64_t>{}(value) + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2); };
        mix(key.cullMode);
        mix(key.polygonMode);
        mix(key.topology);
        mix(key.frontFace);
        mix((uint64_t(key.baseColorTexture.mIndex) << 32) | key.baseColorTexture.mGeneration);
        mix((uint64_t(key.normalTexture.mIndex) << 32) | key.normalTexture.mGeneration);
        mix(key.sampler.magFilter);
        mix(key.sampler.minFilter);
        mix(key.sampler.mipmapMode);
        mix(key.sampler.addressModeU);
        mix(key.sampler.addressModeV);
        mix(key.sampler.useMips);
        mix(std::bit_cast<uint32_t>(key.sampler.maxAnisotropy));
        mix(static_cast<uint64_t>(key.alphaMode));
        mix(std::bit_cast<uint32_t>(key.alphaCutoff));
        for (const std::byte byte : key.record)
            mix(static_cast<uint64_t>(byte));
        mix(key.recordAlignment);
        mix(key.surfaceMaterial);
        mix(std::hash<std::string>{}(key.surfaceAsset));
        return hash;
    }

    uint64_t MaterialRecordCache::Acquire(const MaterialDesc &desc, std::vector<TextureLoadLease> claims)
    {
        MaterialValueKey key = MaterialValueKey::Of(desc);
        if (const auto found = mIds.find(key); found != mIds.end())
            return found->second;
        const MaterialHandle handle = mHost.CreateMaterial(desc);
        if (handle.IsNull())
            return 0;
        const uint64_t id = mNextId++;
        Record &record = mRecords[id];
        record.handle = handle;
        record.claims = std::move(claims);
        record.key = key;
        mIds.emplace(std::move(key), id);
        return id;
    }

    void MaterialRecordCache::Retain(uint64_t id)
    {
        if (const auto found = mRecords.find(id); found != mRecords.end())
        {
            ++found->second.consumers;
            found->second.lastUse = ++mUseSerial;
        }
    }

    void MaterialRecordCache::Drop(uint64_t id)
    {
        const auto found = mRecords.find(id);
        if (found == mRecords.end() || found->second.consumers == 0)
            return;
        if (--found->second.consumers == 0)
        {
            found->second.lastUse = ++mUseSerial;
            Trim();
        }
    }

    MaterialHandle MaterialRecordCache::Find(uint64_t id) const
    {
        const auto found = mRecords.find(id);
        return found == mRecords.end() ? MaterialHandle{} : found->second.handle;
    }

    void MaterialRecordCache::Trim()
    {
        GpuResidency &residency = mHost.Residency();
        if (residency.Excess(GpuCategory::Texture) == 0 && residency.Excess(GpuCategory::Mesh) == 0)
            return;
        EvictUnused();
    }

    void MaterialRecordCache::EvictUnused()
    {
        std::vector<std::pair<uint64_t, uint64_t>> unused;
        for (const auto &[id, record] : mRecords)
            if (record.consumers == 0)
                unused.emplace_back(record.lastUse, id);
        std::sort(unused.begin(), unused.end());
        for (const auto &[serial, id] : unused)
            Evict(mRecords.find(id));
    }

    void MaterialRecordCache::Evict(std::unordered_map<uint64_t, Record>::iterator record)
    {
        const MaterialHandle handle = record->second.handle;
        mIds.erase(record->second.key);
        // The claims drop after the move out, so a texture can retire as its last material goes.
        std::vector<TextureLoadLease> claims = std::move(record->second.claims);
        mRecords.erase(record);
        mHost.ReleaseModelResources({}, std::span(&handle, 1), {});
        claims.clear();
    }

    void MaterialRecordCache::Reset()
    {
        std::unordered_map<uint64_t, Record> records = std::move(mRecords);
        mRecords.clear();
        mIds.clear();
        records.clear();
    }

    MaterialLease &MaterialLease::operator=(MaterialLease &&other) noexcept
    {
        if (this != &other)
        {
            Reset();
            mCache = std::move(other.mCache);
            mId = other.mId;
            other.mId = 0;
        }
        return *this;
    }

    void MaterialLease::Reset()
    {
        if (mCache && mId != 0)
            mCache->Drop(mId);
        mCache.reset();
        mId = 0;
    }
}
