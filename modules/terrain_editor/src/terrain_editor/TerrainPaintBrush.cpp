/**
 * @file TerrainPaintBrush.cpp
 * @author Jackey Cen
 * @brief Paints a material layer's weight channels into one Edit Layer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainPaintBrush.h>

#include <terrain/TerrainEditLayer.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        int64_t FloorDiv(int64_t value, int64_t divisor)
        {
            int64_t quotient = value / divisor;
            if (value % divisor != 0 && (value < 0) != (divisor < 0))
                --quotient;
            return quotient;
        }

        double FalloffWeight(double distance, double radius, double falloff)
        {
            const double t = std::clamp(distance / radius, 0.0, 1.0);
            const double edge = 1.0 - std::clamp(falloff, 0.0, 1.0);
            if (t <= edge || edge >= 1.0)
                return 1.0;
            const double u = (t - edge) / (1.0 - edge);
            return 1.0 - u * u * (3.0 - 2.0 * u);
        }

        bool SameFloatBits(float left, float right)
        {
            return std::bit_cast<uint32_t>(left) == std::bit_cast<uint32_t>(right);
        }

        bool SameTileBits(const std::optional<TerrainEditTile> &left, const std::optional<TerrainEditTile> &right)
        {
            if (left.has_value() != right.has_value())
                return false;
            if (!left)
                return true;
            if (left->tile != right->tile || left->intervals != right->intervals ||
                left->values.size() != right->values.size() || left->alpha.size() != right->alpha.size())
                return false;
            for (std::size_t i = 0; i < left->values.size(); ++i)
            {
                if (!SameFloatBits(left->values[i], right->values[i]))
                    return false;
            }
            for (std::size_t i = 0; i < left->alpha.size(); ++i)
            {
                if (!SameFloatBits(left->alpha[i], right->alpha[i]))
                    return false;
            }
            if (left->weights.size() != right->weights.size())
                return false;
            for (std::size_t channel = 0; channel < left->weights.size(); ++channel)
            {
                if (left->weights[channel].layer != right->weights[channel].layer ||
                    left->weights[channel].values.size() != right->weights[channel].values.size())
                    return false;
                for (std::size_t i = 0; i < left->weights[channel].values.size(); ++i)
                    if (!SameFloatBits(left->weights[channel].values[i], right->weights[channel].values[i]))
                        return false;
            }
            return true;
        }

        // The tiles SetTerrainEditWeightSample writes a shared sample into.
        std::array<TileCoord, 4> HoldingTiles(const TerrainLayout &layout, GlobalSampleIndex sample, std::size_t &count)
        {
            const int32_t intervals = layout.IntervalsPerTile();
            const int64_t firstX = FloorDiv(sample.x, intervals);
            const int64_t firstZ = FloorDiv(sample.z, intervals);

            std::array<int64_t, 2> tilesX{ firstX, firstX };
            std::array<int64_t, 2> tilesZ{ firstZ, firstZ };
            std::size_t countX = 1;
            std::size_t countZ = 1;
            if (sample.x - firstX * intervals == 0)
                tilesX[countX++] = firstX - 1;
            if (sample.z - firstZ * intervals == 0)
                tilesZ[countZ++] = firstZ - 1;

            std::array<TileCoord, 4> tiles{};
            count = 0;
            const TileRange range = layout.Tiles();
            for (std::size_t ix = 0; ix < countX; ++ix)
            {
                for (std::size_t iz = 0; iz < countZ; ++iz)
                {
                    if (tilesX[ix] < range.min.x || tilesX[ix] > range.max.x || tilesZ[iz] < range.min.z ||
                        tilesZ[iz] > range.max.z)
                        continue;
                    tiles[count++] = { static_cast<int32_t>(tilesX[ix]), static_cast<int32_t>(tilesZ[iz]) };
                }
            }
            return tiles;
        }

        std::size_t SampleIndex(const TerrainEditTile &tile, int32_t intervals, GlobalSampleIndex sample)
        {
            const int64_t localX = sample.x - static_cast<int64_t>(tile.tile.x) * intervals;
            const int64_t localZ = sample.z - static_cast<int64_t>(tile.tile.z) * intervals;
            return static_cast<std::size_t>(localZ) * static_cast<std::size_t>(intervals + 1) +
                   static_cast<std::size_t>(localX);
        }

        std::optional<float> ChannelAt(const TerrainEditLayer &layer, std::span<const TileCoord> held,
                                       GlobalSampleIndex sample, int32_t intervals, std::string_view name)
        {
            for (const TileCoord &coord : held)
            {
                const auto tile = layer.tiles.find(coord);
                if (tile == layer.tiles.end())
                    continue;
                const auto channel = std::find_if(tile->second.weights.begin(), tile->second.weights.end(),
                                                  [name](const TerrainEditWeight &weight)
                                                  {
                                                      return weight.layer == name;
                                                  });
                if (channel == tile->second.weights.end())
                    continue;
                const std::size_t index = SampleIndex(tile->second, intervals, sample);
                if (index >= channel->values.size())
                    continue;
                return channel->values[index];
            }
            return std::nullopt;
        }
    }

    TerrainPaintStroke::TerrainPaintStroke(TerrainEditLayer &layer, TerrainLayout reference,
                                           std::string materialLayer, std::function<void()> changed)
        : mLayer(layer), mReference(std::move(reference)), mMaterialLayer(std::move(materialLayer)),
          mChanged(std::move(changed))
    {
    }

    TerrainBrushDabResult TerrainPaintStroke::Dab(TerrainLocalPosition center,
                                                  const TerrainBrushSettings &settings, double seconds)
    {
        TerrainBrushDabResult result;
        const double radius = settings.radius;
        if (settings.brush != TerrainBrush::Paint || mMaterialLayer.empty() || !(radius > 0.0) || !(seconds > 0.0) ||
            !std::isfinite(center.Meters().x) || !std::isfinite(center.Meters().z))
            return result;

        const double spacing = mReference.SampleSpacing();
        const TileRange tiles = mReference.Tiles();
        const int32_t intervals = mReference.IntervalsPerTile();

        const auto tileOf = [&](double value) {
            return static_cast<int32_t>(mReference.TileOfCell(static_cast<int64_t>(std::floor(value / spacing))));
        };
        const auto clampTile = [&](TileCoord tile) {
            return TileCoord{ std::clamp(tile.x, tiles.min.x, tiles.max.x),
                              std::clamp(tile.z, tiles.min.z, tiles.max.z) };
        };

        const TileCoord minTile = clampTile({ tileOf(center.Meters().x - radius),
                                              tileOf(center.Meters().z - radius) });
        const TileCoord maxTile = clampTile({ tileOf(center.Meters().x + radius),
                                              tileOf(center.Meters().z + radius) });

        const double centerX = center.Meters().x;
        const double centerZ = center.Meters().z;
        const double radiusSquared = radius * radius;
        const double strength = std::max(0.0, settings.strength);
        const bool erase = settings.lower;

        TerrainEditTileStates tentative;
        std::set<std::pair<int64_t, int64_t>> visited;

        for (int32_t z = minTile.z; z <= maxTile.z; ++z)
        {
            for (int32_t x = minTile.x; x <= maxTile.x; ++x)
            {
                const TileCoord coord{ x, z };
                const std::optional<SampleRange> samples = mReference.TileSamples(coord);
                if (!samples)
                    continue;

                const SampleRange clipped = ClipBrushSamples(mReference, *samples, centerX, centerZ, radius);
                for (int64_t sampleZ = clipped.min.z; sampleZ <= clipped.max.z; ++sampleZ)
                {
                    for (int64_t sampleX = clipped.min.x; sampleX <= clipped.max.x; ++sampleX)
                    {
                        const GlobalSampleIndex sample{ sampleX, sampleZ };
                        if (mReference.IsPaddingSample(sample))
                            continue;
                        const double dx = mReference.SamplePosition(sampleX) - centerX;
                        const double dz = mReference.SamplePosition(sampleZ) - centerZ;
                        const double distanceSquared = dx * dx + dz * dz;
                        if (distanceSquared > radiusSquared)
                            continue;
                        // A shared edge sample is written to all its tiles in one call.
                        if (!visited.emplace(sampleX, sampleZ).second)
                            continue;

                        const double weight =
                            std::min(1.0, strength * FalloffWeight(std::sqrt(distanceSquared), radius, settings.falloff) *
                                              seconds * kTerrainPaintRatePerSecond);
                        if (!(weight > 0.0))
                            continue;

                        std::size_t heldCount = 0;
                        const std::array<TileCoord, 4> held = HoldingTiles(mReference, sample, heldCount);
                        const std::span<const TileCoord> holders(held.data(), heldCount);

                        std::vector<std::string> names;
                        names.push_back(mMaterialLayer);
                        for (const TileCoord &tileCoord : holders)
                        {
                            const auto tile = mLayer.tiles.find(tileCoord);
                            if (tile == mLayer.tiles.end())
                                continue;
                            for (const TerrainEditWeight &channel : tile->second.weights)
                                if (std::find(names.begin(), names.end(), channel.layer) == names.end())
                                    names.push_back(channel.layer);
                        }

                        std::vector<std::pair<std::string, float>> updates;
                        for (const std::string &name : names)
                        {
                            const std::optional<float> old =
                                ChannelAt(mLayer, holders, sample, intervals, name);
                            const double current = old ? static_cast<double>(*old) : 0.0;
                            double next = current;
                            if (name == mMaterialLayer)
                                next = erase ? current * (1.0 - weight) : current + weight * (1.0 - current);
                            else if (!erase)
                                next = current * (1.0 - weight);

                            const float value = static_cast<float>(next);
                            if (!std::isfinite(next) || (old && SameFloatBits(*old, value)))
                                continue;
                            if (!old && SameFloatBits(value, 0.0f))
                                continue;
                            updates.emplace_back(name, value);
                        }
                        if (updates.empty())
                            continue;

                        for (std::size_t i = 0; i < heldCount; ++i)
                        {
                            if (tentative.contains(held[i]))
                                continue;
                            const auto tile = mLayer.tiles.find(held[i]);
                            tentative.emplace(held[i], tile == mLayer.tiles.end()
                                                           ? std::nullopt
                                                           : std::optional<TerrainEditTile>(tile->second));
                        }
                        for (const auto &[name, value] : updates)
                            SetTerrainEditWeightSample(mLayer, mReference, sample, name, value);
                    }
                }
            }
        }

        for (const auto &[tile, before] : tentative)
        {
            const auto current = mLayer.tiles.find(tile);
            const std::optional<TerrainEditTile> after =
                current == mLayer.tiles.end() ? std::nullopt : std::optional<TerrainEditTile>(current->second);
            if (SameTileBits(before, after))
                continue;
            result.changed.push_back(tile);
            if (!mBefore.contains(tile))
                mBefore.emplace(tile, before);
        }
        if (!result.changed.empty() && mChanged)
            mChanged();
        return result;
    }

    TerrainEditTileStates TerrainPaintStroke::After() const
    {
        TerrainEditTileStates after;
        for (const auto &[tile, before] : mBefore)
        {
            (void)before;
            const auto found = mLayer.tiles.find(tile);
            after.emplace(tile, found == mLayer.tiles.end() ? std::nullopt
                                                            : std::optional<TerrainEditTile>(found->second));
        }
        return after;
    }
} // namespace mir
