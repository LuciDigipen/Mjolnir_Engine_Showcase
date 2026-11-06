/**
 * @file TerrainHeightSource.cpp
 * @author Rahul Nair
 * @brief Authored heights behind the "mir.heightmap" generator, read from an MHGT file.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainHeightSource.h"

#include "TerrainRecipe.h"
#include "TerrainReport.h"

#include <assets/AssetId.h>
#include <core/fs/FileIO.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <span>
#include <system_error>

namespace mir::terrainbake
{
    namespace
    {
        uint32_t LoadU32(std::span<const std::byte> bytes, size_t at)
        {
            uint32_t value = 0;
            for (int i = 0; i < 4; ++i)
                value |= static_cast<uint32_t>(bytes[at + i]) << (8 * i);
            return value;
        }

        double LoadF64(std::span<const std::byte> bytes, size_t at)
        {
            uint64_t value = 0;
            for (int i = 0; i < 8; ++i)
                value |= static_cast<uint64_t>(bytes[at + i]) << (8 * i);
            return std::bit_cast<double>(value);
        }

        // The sample whose SamplePosition is exactly this coordinate, if any.
        std::optional<int64_t> SampleIndexOf(const TerrainLayout &layout, double coordinate)
        {
            constexpr double kExactIntegerLimit = 9007199254740992.0; // 2^53
            const double index = std::round(coordinate / layout.SampleSpacing());
            if (!(std::abs(index) < kExactIntegerLimit) || layout.SamplePosition(int64_t(index)) != coordinate)
                return std::nullopt;
            return int64_t(index);
        }

        // On-grid coordinates divide to within rounding of an integer; that must stay an exact read.
        double SnapToSample(double position)
        {
            constexpr double kSnapDistance = 1e-9;
            const double nearest = std::round(position);
            return std::abs(position - nearest) < kSnapDistance ? nearest : position;
        }
    }

    std::shared_ptr<const TerrainHeightSource> TerrainHeightSource::Load(const std::filesystem::path &path,
                                                                         uint64_t expectedHash,
                                                                         const TerrainLayout &layout, std::string &error)
    {
        const std::string name = PathUtf8(path);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            error = std::format("height source {} does not exist", name);
            return nullptr;
        }
        const std::optional<std::vector<std::byte>> file = ReadBytes(path);
        if (!file)
        {
            error = std::format("cannot read height source {}", name);
            return nullptr;
        }
        const std::span<const std::byte> bytes(*file);
        if (bytes.size() < kHeightSourceHeaderSize ||
            LoadU32(bytes, 0) != (uint32_t('M') | uint32_t('H') << 8 | uint32_t('G') << 16 | uint32_t('T') << 24))
        {
            error = std::format("{} is not an MHGT height source", name);
            return nullptr;
        }
        if (const uint32_t version = LoadU32(bytes, 4); version != kHeightSourceVersion)
        {
            error = std::format("{} is version {}; this tool reads {}", name, version, kHeightSourceVersion);
            return nullptr;
        }

        std::shared_ptr<TerrainHeightSource> source(new TerrainHeightSource());
        source->mWidth = LoadU32(bytes, 8);
        source->mHeight = LoadU32(bytes, 12);
        source->mOriginX = LoadF64(bytes, 16);
        source->mOriginZ = LoadF64(bytes, 24);
        source->mSpacing = LoadF64(bytes, 32);
        const uint64_t samples = uint64_t(source->mWidth) * source->mHeight;
        if (source->mWidth < 2 || source->mHeight < 2 || samples > (bytes.size() - kHeightSourceHeaderSize) / 4 ||
            bytes.size() != kHeightSourceHeaderSize + samples * 4)
        {
            error = std::format("{}: {} x {} samples do not match its {} bytes", name, source->mWidth, source->mHeight,
                                bytes.size());
            return nullptr;
        }
        if (const uint64_t hash = Fnv1a64(bytes); hash != expectedHash)
        {
            error = std::format("{} hashes to {}, the recipe's contentHash is {}", name, TerrainIdHex(hash),
                                TerrainIdHex(expectedHash));
            return nullptr;
        }

        // Sample positions must land on the grid, or a bake would silently interpolate.
        const double spacing = layout.SampleSpacing();
        if (source->mSpacing != spacing)
        {
            error = std::format("{}: spacing {} m differs from the recipe's {} m", name, source->mSpacing, spacing);
            return nullptr;
        }
        // Compared as indices: a decimal spacing makes a remainder or a summed end position inexact.
        const std::optional<int64_t> originX = SampleIndexOf(layout, source->mOriginX);
        const std::optional<int64_t> originZ = SampleIndexOf(layout, source->mOriginZ);
        if (!originX || !originZ)
        {
            error = std::format("{}: origin ({}, {}) is off the {} m sample grid", name, source->mOriginX,
                                source->mOriginZ, spacing);
            return nullptr;
        }
        const TileRange tiles = layout.Tiles();
        const int64_t firstX = layout.TileFirstSample(tiles.min.x);
        const int64_t firstZ = layout.TileFirstSample(tiles.min.z);
        const int64_t lastX = layout.TileFirstSample(tiles.max.x) + layout.IntervalsPerTile();
        const int64_t lastZ = layout.TileFirstSample(tiles.max.z) + layout.IntervalsPerTile();
        const int64_t endX = *originX + source->mWidth - 1;
        const int64_t endZ = *originZ + source->mHeight - 1;
        if (*originX > firstX || *originZ > firstZ || endX < lastX || endZ < lastZ)
        {
            error = std::format("{} covers samples X {} to {}, Z {} to {}; the map's tiles need X {} to {}, Z {} to {}",
                                name, *originX, endX, *originZ, endZ, firstX, lastX, firstZ, lastZ);
            return nullptr;
        }

        source->mHeights.resize(size_t(samples));
        for (size_t i = 0; i < source->mHeights.size(); ++i)
        {
            const float height = std::bit_cast<float>(LoadU32(bytes, kHeightSourceHeaderSize + i * 4));
            if (!std::isfinite(height) || std::abs(height) > kMaxHeightMagnitude)
            {
                error = std::format("{}: sample {} is {}, outside finite +-{} m", name, i, height, kMaxHeightMagnitude);
                return nullptr;
            }
            source->mHeights[i] = height;
        }
        return source;
    }

    double TerrainHeightSource::Sample(double x, double z) const
    {
        const double fx = std::clamp(SnapToSample((x - mOriginX) / mSpacing), 0.0, double(mWidth - 1));
        const double fz = std::clamp(SnapToSample((z - mOriginZ) / mSpacing), 0.0, double(mHeight - 1));
        const uint32_t i = std::min(uint32_t(fx), mWidth - 2);
        const uint32_t k = std::min(uint32_t(fz), mHeight - 2);
        const double tx = fx - i;
        const double tz = fz - k;
        const float *row = mHeights.data() + size_t(k) * mWidth;
        const double lower = double(row[i]) * (1.0 - tx) + double(row[i + 1]) * tx;
        const double upper = double(row[mWidth + i]) * (1.0 - tx) + double(row[mWidth + i + 1]) * tx;
        return lower * (1.0 - tz) + upper * tz;
    }
}
