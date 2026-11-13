/**
 * @file TerrainHeightBrush.cpp
 * @author Jackey Cen
 * @brief Applies a height brush's dabs to one Edit Layer and records the tiles it changed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainHeightBrush.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TileHeights.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <set>
#include <tuple>
#include <utility>

namespace mir
{
    namespace
    {
        struct TileContext
        {
            TerrainLayout layout;
            TerrainBaseTile base;
            TileHeights composite;
            std::optional<TileHeights> below;
        };

        using TileContextMap = std::map<TileCoord, TileContext, TerrainTileOrder>;

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

        // The tiles SetTerrainEditSample writes a shared sample into.
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

        std::optional<float> CompositeAt(const TileContextMap &contexts, const TileContext &current, int64_t x,
                                          int64_t z)
        {
            const int32_t intervals = current.layout.IntervalsPerTile();
            const TileCoord owner{ static_cast<int32_t>(FloorDiv(x, intervals)),
                                   static_cast<int32_t>(FloorDiv(z, intervals)) };
            const auto found = contexts.find(owner);
            if (found == contexts.end())
                return std::nullopt;

            const TileContext &context = found->second;
            const int64_t localX = x - static_cast<int64_t>(owner.x) * context.layout.IntervalsPerTile();
            const int64_t localZ = z - static_cast<int64_t>(owner.z) * context.layout.IntervalsPerTile();
            const int32_t side = context.layout.IntervalsPerTile();
            if (localX < 0 || localZ < 0 || localX > side || localZ > side)
                return std::nullopt;
            return context.composite.At(static_cast<int32_t>(localX), static_cast<int32_t>(localZ));
        }
    }

    TerrainBrushStroke::TerrainBrushStroke(TerrainEditLayer &layer, std::span<const TerrainEditLayer> stack,
                                           TerrainBrushTileSource source, double startHeight,
                                           std::function<void()> changed)
        : mLayer(layer), mStack(stack), mSource(std::move(source)), mChanged(std::move(changed)),
          mStartHeight(startHeight)
    {
        const std::vector<const TerrainEditLayer *> order = TerrainEditCompositingOrder(mStack);
        const auto stop = std::find(order.begin(), order.end(), &mLayer);
        const std::size_t count = stop == order.end() ? order.size()
                                                      : static_cast<std::size_t>(stop - order.begin());
        for (std::size_t i = 0; i < count; ++i)
            mBelowStack.push_back(*order[i]);
    }

    TerrainBrushDabResult TerrainBrushStroke::Dab(TerrainLocalPosition center, const TerrainBrushSettings &settings,
                                                  double seconds)
    {
        TerrainBrushDabResult result;
        const double radius = settings.radius;
        if (settings.brush == TerrainBrush::Paint || !(radius > 0.0) || !(seconds > 0.0) ||
            !std::isfinite(center.Meters().x) || !std::isfinite(center.Meters().z))
            return result;

        const double spacing = mSource.reference.SampleSpacing();
        const TileRange tiles = mSource.reference.Tiles();

        const auto tileOf = [&](double value) {
            return static_cast<int32_t>(mSource.reference.TileOfCell(static_cast<int64_t>(std::floor(value / spacing))));
        };
        const auto clampTile = [&](TileCoord tile) {
            return TileCoord{ std::clamp(tile.x, tiles.min.x, tiles.max.x),
                              std::clamp(tile.z, tiles.min.z, tiles.max.z) };
        };

        const TileCoord minTile = clampTile({ tileOf(center.Meters().x - radius),
                                              tileOf(center.Meters().z - radius) });
        const TileCoord maxTile = clampTile({ tileOf(center.Meters().x + radius),
                                              tileOf(center.Meters().z + radius) });
        const TileCoord ringMin = clampTile({ minTile.x - 1, minTile.z - 1 });
        const TileCoord ringMax = clampTile({ maxTile.x + 1, maxTile.z + 1 });

        const auto build = [this](TileCoord tile, std::string &error) -> std::optional<TileContext> {
            std::optional<TerrainLayout> layout = mSource.layout ? mSource.layout(tile) : mSource.reference;
            if (!layout)
            {
                error = std::format("Tile ({}, {}) has no source grid.", tile.x, tile.z);
                return std::nullopt;
            }

            TerrainBaseHeightsResult<TerrainBaseTile> base = mSource.base(tile);
            if (!base || !base.value)
            {
                error = base.error.message.empty() ? std::format("Tile ({}, {}) has no base heights.", tile.x,
                                                                  tile.z)
                                                   : base.error.message;
                return std::nullopt;
            }

            TerrainResult<TileHeights> composite = CompositeTerrainEditLayers(**base.value, mStack);
            if (!composite)
            {
                error = composite.error.message;
                return std::nullopt;
            }
            if (composite.value->IntervalsPerTile() != layout->IntervalsPerTile())
            {
                error = std::format("Tile ({}, {}) has {} base intervals, its source grid has {}.", tile.x, tile.z,
                                    composite.value->IntervalsPerTile(), layout->IntervalsPerTile());
                return std::nullopt;
            }

            std::optional<TileHeights> below;
            if (mLayer.info.heightBlend == TerrainEditBlend::AlphaBlend)
            {
                TerrainResult<TileHeights> prefix = CompositeTerrainEditLayers(**base.value, mBelowStack);
                if (!prefix)
                {
                    error = prefix.error.message;
                    return std::nullopt;
                }
                below = std::move(*prefix.value);
            }
            return TileContext{ *layout, *base.value, std::move(*composite.value), std::move(below) };
        };

        TileContextMap contexts;
        for (int32_t z = ringMin.z;; ++z)
        {
            for (int32_t x = ringMin.x;; ++x)
            {
                const TileCoord tile{ x, z };
                std::string error;
                std::optional<TileContext> context = build(tile, error);
                const bool inBrush = x >= minTile.x && x <= maxTile.x && z >= minTile.z && z <= maxTile.z;
                if (!context && inBrush)
                {
                    result.error = std::move(error);
                    return result;
                }
                if (context)
                    contexts.emplace(tile, std::move(*context));
                if (x == ringMax.x)
                    break;
            }
            if (z == ringMax.z)
                break;
        }

        const bool alphaBlend = mLayer.info.heightBlend == TerrainEditBlend::AlphaBlend;
        TerrainEditTileStates tentative;
        std::set<std::tuple<uint64_t, int64_t, int64_t>> visited;
        const double centerX = center.Meters().x;
        const double centerZ = center.Meters().z;
        const double radiusSquared = radius * radius;

        for (int32_t z = minTile.z; z <= maxTile.z; ++z)
        {
            for (int32_t x = minTile.x; x <= maxTile.x; ++x)
            {
                const TileCoord coord{ x, z };
                const auto found = contexts.find(coord);
                if (found == contexts.end())
                    continue;
                const TileContext &context = found->second;
                const std::optional<SampleRange> samples = context.layout.TileSamples(coord);
                if (!samples)
                    continue;
                const int32_t intervals = context.layout.IntervalsPerTile();

                for (int64_t sampleZ = samples->min.z; sampleZ <= samples->max.z; ++sampleZ)
                {
                    for (int64_t sampleX = samples->min.x; sampleX <= samples->max.x; ++sampleX)
                    {
                        const GlobalSampleIndex sample{ sampleX, sampleZ };
                        if (context.layout.IsPaddingSample(sample))
                            continue;
                        // Shared edges are visited once, or their layer value would take the dab twice.
                        const auto key = std::make_tuple(std::bit_cast<uint64_t>(context.layout.SampleSpacing()),
                                                         sampleX, sampleZ);
                        if (!visited.emplace(key).second)
                            continue;

                        const double dx = context.layout.SamplePosition(sampleX) - centerX;
                        const double dz = context.layout.SamplePosition(sampleZ) - centerZ;
                        const double distanceSquared = dx * dx + dz * dz;
                        if (distanceSquared > radiusSquared)
                            continue;

                        const int64_t localX = sampleX - static_cast<int64_t>(coord.x) * intervals;
                        const int64_t localZ = sampleZ - static_cast<int64_t>(coord.z) * intervals;
                        const double composite = context.composite.At(static_cast<int32_t>(localX),
                                                                      static_cast<int32_t>(localZ));

                        double target = composite;
                        if (settings.brush == TerrainBrush::Flatten)
                            target = mStartHeight;
                        else if (settings.brush == TerrainBrush::Smooth)
                        {
                            double sum = 0.0;
                            int count = 0;
                            const std::array<GlobalSampleIndex, 4> neighbours{ GlobalSampleIndex{ sampleX - 1, sampleZ },
                                GlobalSampleIndex{ sampleX + 1, sampleZ }, GlobalSampleIndex{ sampleX, sampleZ - 1 },
                                GlobalSampleIndex{ sampleX, sampleZ + 1 } };
                            for (const GlobalSampleIndex &neighbour : neighbours)
                            {
                                if (context.layout.IsPaddingSample(neighbour))
                                    continue;
                                const std::optional<float> height =
                                    CompositeAt(contexts, context, neighbour.x, neighbour.z);
                                if (!height)
                                    continue;
                                sum += *height;
                                ++count;
                            }
                            if (count == 0)
                                continue;
                            target = sum / count;
                        }

                        const double weight = std::max(0.0, settings.strength) *
                                              FalloffWeight(std::sqrt(distanceSquared), radius, settings.falloff) *
                                              seconds;
                        if (!(weight > 0.0))
                            continue;

                        double delta = 0.0;
                        if (settings.brush == TerrainBrush::RaiseLower)
                        {
                            target = composite + (settings.lower ? -weight : weight);
                            delta = settings.lower ? -weight : weight;
                        }
                        else
                            delta = std::clamp(target - composite, -weight, weight);
                        if (!std::isfinite(target) || !std::isfinite(delta))
                            continue;

                        float oldValue = 0.0f;
                        float oldAlpha = 0.0f;
                        std::size_t heldCount = 0;
                        const std::array<TileCoord, 4> held = HoldingTiles(context.layout, sample, heldCount);
                        for (std::size_t i = 0; i < heldCount; ++i)
                        {
                            const auto tile = mLayer.tiles.find(held[i]);
                            if (tile == mLayer.tiles.end())
                                continue;
                            const std::size_t index = SampleIndex(tile->second, intervals, sample);
                            if (index >= tile->second.values.size())
                                continue;
                            oldValue = tile->second.values[index];
                            oldAlpha = index < tile->second.alpha.size() ? tile->second.alpha[index] : 0.0f;
                            break;
                        }

                        float newValue = 0.0f;
                        float newAlpha = 0.0f;
                        if (alphaBlend)
                        {
                            const double below = context.below->At(static_cast<int32_t>(localX),
                                                                   static_cast<int32_t>(localZ));
                            const double falloff = FalloffWeight(std::sqrt(distanceSquared), radius, settings.falloff);
                            const double alpha = std::max(static_cast<double>(oldAlpha), falloff);
                            if (!(alpha > 0.0))
                                continue;
                            const double layerNow = below + static_cast<double>(oldAlpha) * (oldValue - below);
                            newValue = static_cast<float>(below + (layerNow + delta - below) / alpha);
                            newAlpha = static_cast<float>(alpha);
                        }
                        else
                            newValue = static_cast<float>(oldValue + delta);

                        bool needsWrite = false;
                        for (std::size_t i = 0; i < heldCount && !needsWrite; ++i)
                        {
                            const auto tile = mLayer.tiles.find(held[i]);
                            if (tile == mLayer.tiles.end())
                            {
                                needsWrite = !SameFloatBits(newValue, 0.0f) ||
                                             (alphaBlend && !SameFloatBits(newAlpha, 0.0f));
                                continue;
                            }
                            const std::size_t index = SampleIndex(tile->second, intervals, sample);
                            if (index >= tile->second.values.size())
                            {
                                needsWrite = !SameFloatBits(newValue, 0.0f) ||
                                             (alphaBlend && !SameFloatBits(newAlpha, 0.0f));
                                continue;
                            }
                            const float value = tile->second.values[index];
                            const float alpha = index < tile->second.alpha.size() ? tile->second.alpha[index] : 0.0f;
                            needsWrite = !SameFloatBits(value, newValue) ||
                                         (alphaBlend && !SameFloatBits(alpha, newAlpha));
                        }
                        if (!needsWrite)
                            continue;

                        for (std::size_t i = 0; i < heldCount; ++i)
                        {
                            if (tentative.contains(held[i]))
                                continue;
                            const auto tile = mLayer.tiles.find(held[i]);
                            tentative.emplace(held[i],
                                              tile == mLayer.tiles.end()
                                                  ? std::nullopt
                                                  : std::optional<TerrainEditTile>(tile->second));
                        }
                        SetTerrainEditSample(mLayer, context.layout, sample, newValue, alphaBlend ? newAlpha : 0.0f);
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

    TerrainEditTileStates TerrainBrushStroke::After() const
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
