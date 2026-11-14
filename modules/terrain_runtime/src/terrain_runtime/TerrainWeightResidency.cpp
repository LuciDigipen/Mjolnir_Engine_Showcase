/**
 * @file TerrainWeightResidency.cpp
 * @author Jackey Cen
 * @brief Keeps one RGBA8 weight texture per requested terrain tile within a GPU budget.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_runtime/TerrainWeightResidency.h>

#include <renderer/TextureUpload.h>

#include <algorithm>
#include <deque>
#include <mutex>
#include <utility>

namespace mir
{
    namespace
    {
        enum class WeightState : uint8_t
        {
            Reading,
            Uploading,
            Ready,
            Failed,
        };

        struct ReadResult
        {
            uint64_t ticket = 0;
            bool ok = false;
            uint32_t side = 0;
            std::array<uint8_t, kTerrainDrawSlots> recordSlots{};
            std::vector<std::byte> bytes;
        };
    }

    struct TerrainWeightResidency::Shared
    {
        std::mutex mutex;
        std::deque<ReadResult> finished;
    };

    struct TerrainWeightResidency::Entry
    {
        uint64_t checksum = 0;
        uint64_t ticket = 0;
        uint64_t bytes = 0;
        float invFootprint = 0.0f;
        float distance = 0.0f;
        double lastRequested = 0.0;
        std::array<uint8_t, kTerrainDrawSlots> recordSlots{};
        WeightState state = WeightState::Reading;
        TextureHandle texture;
    };

    TerrainSlotTable::TerrainSlotTable()
    {
        mLayers.fill(0);
    }

    void TerrainSlotTable::Set(std::span<const uint8_t> layers)
    {
        for (size_t i = 0; i < mLayers.size() && i < layers.size(); ++i)
            mLayers[i] = std::min<uint8_t>(layers[i], kMaxTerrainDrawMaterialLayer);
    }

    std::array<uint8_t, kTerrainDrawSlots> TerrainSlotTable::Map(
        const std::array<uint8_t, kTerrainDrawSlots> &recordSlots) const
    {
        return MapTerrainSlots(recordSlots, mLayers);
    }

    TerrainWeightResidency::TerrainWeightResidency(const TerrainStreamingSettings::Gpu &settings,
                                                   std::shared_ptr<TerrainExecutor> executor, ITextureUploader &textures,
                                                   uint32_t samplerRow)
        : mExecutor(std::move(executor)), mTextures(textures), mShared(std::make_shared<Shared>()),
          mBudgetBytes(settings.weightBudgetBytes), mRetainSeconds(settings.meshRetainSeconds),
          mSamplerRow(samplerRow)
    {
    }

    // The textures belong to the renderer, which may already be gone; Unload retires them while it can.
    TerrainWeightResidency::~TerrainWeightResidency() = default;

    void TerrainWeightResidency::ApplySettings(const TerrainStreamingSettings::Gpu &gpu)
    {
        mBudgetBytes = gpu.weightBudgetBytes;
        mRetainSeconds = gpu.meshRetainSeconds;
    }

    void TerrainWeightResidency::BeginRequest()
    {
        mWants.clear();
    }

    void TerrainWeightResidency::Request(const TerrainWeightWant &want)
    {
        const TileKey key{want.release->Info().asset, want.tile};
        auto found = mWants.find(key);
        if (found == mWants.end())
        {
            mWants.emplace(key, want);
            return;
        }
        if (want.distance < found->second.distance)
            found->second = want;
    }

    uint64_t TerrainWeightResidency::HeldBytes() const
    {
        uint64_t bytes = 0;
        for (const auto &[key, entry] : mEntries)
            if (entry.state != WeightState::Failed)
                bytes += entry.bytes;
        return bytes;
    }

    void TerrainWeightResidency::Retire(Entry &entry)
    {
        if (!entry.texture.IsNull())
        {
            mTextures.RetireTexture(entry.texture);
            ++mRetired;
        }
        entry.texture = kNullTexture;
    }

    void TerrainWeightResidency::MakeRoom(uint64_t bytes, float distance)
    {
        while (HeldBytes() + bytes > mBudgetBytes)
        {
            auto farthest = mEntries.end();
            for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
                if (it->second.distance > distance &&
                    (farthest == mEntries.end() || it->second.distance > farthest->second.distance))
                    farthest = it;
            if (farthest == mEntries.end())
                return;
            Retire(farthest->second);
            mEntries.erase(farthest);
        }
    }

    void TerrainWeightResidency::StartRead(const TileKey &key, const TerrainWeightWant &want,
                                           const TerrainWeightRecord &record, double now)
    {
        const int32_t intervals = want.release->Layout().IntervalsPerTile();
        const uint32_t side = static_cast<uint32_t>(intervals + 1);
        Entry entry;
        entry.checksum = record.checksum;
        entry.ticket = mNextTicket++;
        entry.bytes = uint64_t(side) * side * 4;
        entry.invFootprint = static_cast<float>(1.0 / want.release->Layout().TileSize());
        entry.distance = want.distance;
        entry.lastRequested = now;
        entry.recordSlots = record.slots;
        entry.state = WeightState::Reading;

        const uint64_t ticket = entry.ticket;
        std::shared_ptr<const TerrainReleaseHandle> release = want.release;
        std::shared_ptr<Shared> shared = mShared;
        const TileCoord tile = want.tile;
        mExecutor->Post([shared, release, ticket, tile] {
            ReadResult result;
            result.ticket = ticket;
            TerrainResult<TerrainDecodedWeights> decoded = release->ReadWeights(tile);
            if (decoded)
            {
                result.ok = true;
                result.side = static_cast<uint32_t>(decoded.value->intervals + 1);
                result.recordSlots = decoded.value->slots;
                result.bytes = std::move(decoded.value->bytes);
            }
            std::lock_guard lock(shared->mutex);
            shared->finished.push_back(std::move(result));
        });

        mEntries.insert_or_assign(key, std::move(entry));
    }

    void TerrainWeightResidency::TakeFinished()
    {
        std::deque<ReadResult> finished;
        {
            std::lock_guard lock(mShared->mutex);
            finished.swap(mShared->finished);
        }

        for (ReadResult &result : finished)
        {
            auto found = std::find_if(mEntries.begin(), mEntries.end(), [&](const auto &item) {
                return item.second.ticket == result.ticket && item.second.state == WeightState::Reading;
            });
            if (found == mEntries.end())
                continue;
            Entry &entry = found->second;
            if (!result.ok || result.side == 0)
            {
                entry.state = WeightState::Failed;
                continue;
            }

            const uint64_t held = HeldBytes() - entry.bytes;
            const bool fits = held + result.bytes.size() <= mBudgetBytes;
            if (mSamplerRow == UINT32_MAX || !fits)
            {
                mEntries.erase(found);
                if (!fits)
                    ++mSkippedBudget;
                continue;
            }

            const TextureDesc desc{.width = result.side,
                                   .height = result.side,
                                   .pixels = std::span<const std::byte>(result.bytes),
                                   .srgb = false,
                                   .generateMips = false};
            TextureUploadSource source = PrepareTextureUpload(desc);
            TextureHandle handle = source.desc.width != 0 ? mTextures.QueueTexture(std::move(source))
                                                          : kNullTexture;
            if (handle.IsNull())
            {
                entry.state = WeightState::Failed;
                mEntries.erase(found);
                continue;
            }
            entry.texture = handle;
            entry.bytes = result.bytes.size();
            entry.recordSlots = result.recordSlots;
            entry.state = WeightState::Uploading;
            ++mQueued;
        }
    }

    void TerrainWeightResidency::Install(double now)
    {
        TakeFinished();

        std::vector<TileKey> retired;
        for (auto &[key, entry] : mEntries)
        {
            if (entry.texture.IsNull())
                continue;
            switch (mTextures.TextureState(entry.texture))
            {
            case UploadState::Ready:
                entry.state = WeightState::Ready;
                break;
            case UploadState::Failed:
                Retire(entry);
                retired.push_back(key);
                break;
            case UploadState::Stale:
                entry.texture = kNullTexture;
                retired.push_back(key);
                break;
            case UploadState::Pending:
            case UploadState::Uploading:
                break;
            }
        }
        for (const TileKey &key : retired)
            mEntries.erase(key);

        // Nearest first, so the budget goes to what the viewer sees closest.
        std::vector<std::pair<TileKey, TerrainWeightWant>> wants(mWants.begin(), mWants.end());
        std::sort(wants.begin(), wants.end(), [](const auto &a, const auto &b) {
            if (a.second.distance != b.second.distance)
                return a.second.distance < b.second.distance;
            if (a.first.asset != b.first.asset)
                return a.first.asset.value < b.first.asset.value;
            if (a.first.coord.z != b.first.coord.z)
                return a.first.coord.z < b.first.coord.z;
            return a.first.coord.x < b.first.coord.x;
        });
        std::vector<std::pair<TileKey, TerrainWeightWant>> reads;
        for (const auto &[key, want] : wants)
        {
            const TerrainWeightRecord *record = want.release->Index().FindWeight(want.tile);
            auto found = mEntries.find(key);
            if (!record)
            {
                if (found != mEntries.end())
                {
                    Retire(found->second);
                    mEntries.erase(found);
                }
                continue;
            }
            if (found != mEntries.end())
            {
                if (found->second.checksum == record->checksum)
                {
                    found->second.recordSlots = record->slots;
                    found->second.invFootprint = static_cast<float>(1.0 / want.release->Layout().TileSize());
                    found->second.distance = want.distance;
                    found->second.lastRequested = now;
                    continue;
                }
                Retire(found->second);
                mEntries.erase(found);
            }
            reads.push_back({key, want});
        }

        // A texture outlives the last frame that drew its tile by the mesh retain time, so turning the camera
        // does not re-read and re-upload it.
        std::vector<TileKey> expired;
        for (const auto &[key, entry] : mEntries)
            if (!mWants.contains(key) && now - entry.lastRequested > mRetainSeconds)
                expired.push_back(key);
        for (const TileKey &key : expired)
        {
            Retire(mEntries.at(key));
            mEntries.erase(key);
        }

        for (const auto &[key, want] : reads)
        {
            const TerrainWeightRecord *record = want.release->Index().FindWeight(want.tile);
            if (!record)
                continue;
            const uint32_t side = static_cast<uint32_t>(want.release->Layout().IntervalsPerTile() + 1);
            const uint64_t bytes = uint64_t(side) * side * 4;
            if (mSamplerRow == UINT32_MAX)
            {
                ++mSkippedBudget;
                continue;
            }
            MakeRoom(bytes, want.distance);
            if (HeldBytes() + bytes > mBudgetBytes)
            {
                ++mSkippedBudget;
                continue;
            }
            StartRead(key, want, *record, now);
        }
    }

    std::array<uint32_t, kTerrainDrawWords> TerrainWeightResidency::Words(TerrainUnitKind kind, const TileKey &key,
                                                                         const TerrainSlotTable &slotTable) const
    {
        if (kind != TerrainUnitKind::Tile || mSamplerRow == UINT32_MAX)
            return kNoTerrainDrawWords;
        const auto found = mEntries.find(key);
        if (found == mEntries.end() || found->second.state != WeightState::Ready ||
            found->second.texture.IsNull())
            return kNoTerrainDrawWords;
        TerrainDrawData data;
        data.weightImageRow = found->second.texture.mIndex;
        data.weightSamplerRow = mSamplerRow;
        data.slots = slotTable.Map(found->second.recordSlots);
        data.invFootprint = found->second.invFootprint;
        return data.Pack();
    }

    void TerrainWeightResidency::RetireAll()
    {
        for (auto &[key, entry] : mEntries)
            Retire(entry);
        mEntries.clear();
        mWants.clear();
    }

    TerrainWeightStats TerrainWeightResidency::Stats() const
    {
        TerrainWeightStats stats;
        stats.budgetBytes = mBudgetBytes;
        stats.residentBytes = HeldBytes();
        stats.queued = mQueued;
        stats.retired = mRetired;
        stats.skippedBudget = mSkippedBudget;
        for (const auto &[key, entry] : mEntries)
        {
            switch (entry.state)
            {
            case WeightState::Reading:
                ++stats.reading;
                break;
            case WeightState::Uploading:
                ++stats.uploading;
                break;
            case WeightState::Ready:
                ++stats.ready;
                break;
            case WeightState::Failed:
                ++stats.failed;
                break;
            }
        }
        return stats;
    }
}
