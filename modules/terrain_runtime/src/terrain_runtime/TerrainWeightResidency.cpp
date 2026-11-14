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
#include <optional>
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
            TerrainWeightUnit unit;
            uint64_t ticket = 0;
            bool ok = false;
            uint32_t side = 0;
            std::array<uint8_t, kTerrainDrawSlots> recordSlots{};
            std::vector<std::byte> bytes;
        };

        // What the index says about one unit's image before it is read.
        struct WeightSource
        {
            uint64_t checksum = 0;
            std::array<uint8_t, kTerrainDrawSlots> slots{};
            uint32_t side = 0;
            float invFootprint = 0.0f;
        };

        std::optional<WeightSource> FindWeightSource(const TerrainReleaseHandle &release, const TerrainWeightUnit &unit)
        {
            const TerrainIndex &index = release.Index();
            WeightSource source;
            if (unit.kind == TerrainUnitKind::Region)
            {
                if (!index.regionGrid)
                    return std::nullopt;
                const TerrainRegionCoord region{unit.level, unit.coord.x, unit.coord.z};
                const TerrainRegionWeightRecord *record = index.FindRegionWeight(region);
                if (!record)
                    return std::nullopt;
                const TerrainLocalBounds rect = index.regionGrid->Rect(region);
                const double span = rect.maxX - rect.minX;
                if (!(span > 0.0))
                    return std::nullopt;
                source.checksum = record->checksum;
                source.slots = record->slots;
                source.side = record->side;
                source.invFootprint = static_cast<float>(1.0 / span);
                return source;
            }
            const TerrainWeightRecord *record = index.FindWeight(unit.coord);
            if (!record)
                return std::nullopt;
            source.checksum = record->checksum;
            source.slots = record->slots;
            source.side = static_cast<uint32_t>(index.layout.IntervalsPerTile() + 1);
            source.invFootprint = static_cast<float>(1.0 / index.layout.TileSize());
            return source;
        }

        int RankOrder(TerrainUnitKind kind)
        {
            return kind == TerrainUnitKind::Region ? 0 : 1;
        }
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
        auto found = mWants.find(want.unit);
        if (found == mWants.end())
        {
            mWants.emplace(want.unit, want);
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

    void TerrainWeightResidency::MakeRoom(uint64_t bytes, TerrainUnitKind kind, float distance)
    {
        const int incoming = RankOrder(kind);
        while (HeldBytes() + bytes > mBudgetBytes)
        {
            auto farthest = mEntries.end();
            for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
            {
                const int order = RankOrder(it->first.kind);
                const bool behind = order > incoming || (order == incoming && it->second.distance > distance);
                if (!behind)
                    continue;
                if (farthest == mEntries.end() || order > RankOrder(farthest->first.kind) ||
                    (order == RankOrder(farthest->first.kind) && it->second.distance > farthest->second.distance))
                    farthest = it;
            }
            if (farthest == mEntries.end())
                return;
            Retire(farthest->second);
            mEntries.erase(farthest);
        }
    }

    void TerrainWeightResidency::StartRead(const TerrainWeightWant &want, uint32_t side, uint64_t checksum,
                                           const std::array<uint8_t, kTerrainDrawSlots> &recordSlots, float invFootprint,
                                           double now)
    {
        Entry entry;
        entry.checksum = checksum;
        entry.ticket = mNextTicket++;
        entry.bytes = uint64_t(side) * side * 4;
        entry.invFootprint = invFootprint;
        entry.distance = want.distance;
        entry.lastRequested = now;
        entry.recordSlots = recordSlots;
        entry.state = WeightState::Reading;

        const uint64_t ticket = entry.ticket;
        std::shared_ptr<const TerrainReleaseHandle> release = want.release;
        std::shared_ptr<Shared> shared = mShared;
        const TerrainWeightUnit unit = want.unit;
        mExecutor->Post([shared, release, ticket, unit] {
            ReadResult result;
            result.unit = unit;
            result.ticket = ticket;
            TerrainResult<TerrainDecodedWeights> decoded =
                unit.kind == TerrainUnitKind::Region
                    ? release->ReadRegionWeights({unit.level, unit.coord.x, unit.coord.z})
                    : release->ReadWeights(unit.coord);
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

        mEntries.insert_or_assign(want.unit, std::move(entry));
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
            auto found = mEntries.find(result.unit);
            if (found == mEntries.end() || found->second.ticket != result.ticket ||
                found->second.state != WeightState::Reading)
                continue;
            if (!result.ok || result.side == 0)
            {
                found->second.state = WeightState::Failed;
                continue;
            }

            Entry entry = std::move(found->second);
            mEntries.erase(found);
            if (mSamplerRow == UINT32_MAX)
                continue;
            MakeRoom(result.bytes.size(), result.unit.kind, entry.distance);
            if (HeldBytes() + result.bytes.size() > mBudgetBytes)
            {
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
                continue;
            entry.texture = handle;
            entry.bytes = result.bytes.size();
            entry.recordSlots = result.recordSlots;
            entry.state = WeightState::Uploading;
            ++mQueued;
            mEntries.emplace(result.unit, std::move(entry));
        }
    }

    void TerrainWeightResidency::Install(double now)
    {
        TakeFinished();

        std::vector<TerrainWeightUnit> retired;
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
        for (const TerrainWeightUnit &key : retired)
            mEntries.erase(key);

        // Region images go ahead of every tile, then the nearest unit, so budget pressure reaches tiles first.
        std::vector<std::pair<TerrainWeightUnit, TerrainWeightWant>> wants(mWants.begin(), mWants.end());
        std::sort(wants.begin(), wants.end(), [](const auto &a, const auto &b) {
            if (a.first.kind != b.first.kind)
                return RankOrder(a.first.kind) < RankOrder(b.first.kind);
            if (a.second.distance != b.second.distance)
                return a.second.distance < b.second.distance;
            if (a.first.asset != b.first.asset)
                return a.first.asset.value < b.first.asset.value;
            if (a.first.level != b.first.level)
                return a.first.level < b.first.level;
            if (a.first.coord.z != b.first.coord.z)
                return a.first.coord.z < b.first.coord.z;
            return a.first.coord.x < b.first.coord.x;
        });
        std::vector<std::pair<TerrainWeightUnit, TerrainWeightWant>> reads;
        for (const auto &[key, want] : wants)
        {
            const std::optional<WeightSource> source = FindWeightSource(*want.release, key);
            auto found = mEntries.find(key);
            if (!source)
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
                if (found->second.checksum == source->checksum)
                {
                    found->second.recordSlots = source->slots;
                    found->second.invFootprint = source->invFootprint;
                    found->second.distance = want.distance;
                    found->second.lastRequested = now;
                    continue;
                }
                Retire(found->second);
                mEntries.erase(found);
            }
            reads.push_back({key, want});
        }

        // A texture outlives the last frame that drew its unit by the mesh retain time, so turning the camera
        // does not re-read and re-upload it.
        std::vector<TerrainWeightUnit> expired;
        for (const auto &[key, entry] : mEntries)
            if (!mWants.contains(key) && now - entry.lastRequested > mRetainSeconds)
                expired.push_back(key);
        for (const TerrainWeightUnit &key : expired)
        {
            Retire(mEntries.at(key));
            mEntries.erase(key);
        }

        for (const auto &[key, want] : reads)
        {
            const std::optional<WeightSource> source = FindWeightSource(*want.release, key);
            if (!source)
                continue;
            const uint64_t bytes = uint64_t(source->side) * source->side * 4;
            if (mSamplerRow == UINT32_MAX)
            {
                ++mSkippedBudget;
                continue;
            }
            MakeRoom(bytes, key.kind, want.distance);
            if (HeldBytes() + bytes > mBudgetBytes)
            {
                ++mSkippedBudget;
                continue;
            }
            StartRead(want, source->side, source->checksum, source->slots, source->invFootprint, now);
        }
    }

    std::array<uint32_t, kTerrainDrawWords> TerrainWeightResidency::Words(const TerrainWeightUnit &unit,
                                                                         const TerrainSlotTable &slotTable) const
    {
        if (mSamplerRow == UINT32_MAX)
            return kNoTerrainDrawWords;
        const auto found = mEntries.find(unit);
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
            if (key.kind == TerrainUnitKind::Region && entry.state != WeightState::Failed)
                ++stats.residentRegions;
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
