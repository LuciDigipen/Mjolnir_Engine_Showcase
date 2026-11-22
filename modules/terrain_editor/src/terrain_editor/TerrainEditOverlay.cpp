/**
 * @file TerrainEditOverlay.cpp
 * @author Jackey Cen
 * @brief Composites the tiles a session edited into one overlay snapshot.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainEditOverlay.h>

#include <terrain/TerrainEditLayer.h>
#include <terrain/TerrainPackWriter.h>
#include <terrain/TerrainWeights.h>
#include <terrain/TileHeights.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <format>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr uint64_t kHashOffset = 14695981039346656037ull;
        constexpr uint64_t kHashPrime = 1099511628211ull;

        uint64_t MixBytes(uint64_t hash, std::string_view bytes)
        {
            for (const char byte : bytes)
            {
                hash ^= static_cast<uint64_t>(static_cast<unsigned char>(byte));
                hash *= kHashPrime;
            }
            return hash;
        }

        uint64_t MixBits(uint64_t hash, uint32_t bits)
        {
            for (int shift = 0; shift < 32; shift += 8)
            {
                hash ^= static_cast<uint64_t>((bits >> shift) & 0xFFu);
                hash *= kHashPrime;
            }
            return hash;
        }

        // A tile's paint channels in compositing order plus the base weights it composites onto.
        uint64_t TileWeightHash(std::span<const TerrainEditLayer> layers, TileCoord tile, uint64_t baseWeightsKey)
        {
            uint64_t hash = MixBits(kHashOffset, static_cast<uint32_t>(baseWeightsKey));
            hash = MixBits(hash, static_cast<uint32_t>(baseWeightsKey >> 32));
            for (const TerrainEditLayer *layer : TerrainEditCompositingOrder(layers))
            {
                const auto found = layer->tiles.find(tile);
                if (found == layer->tiles.end() || found->second.weights.empty())
                    continue;
                hash = MixBytes(hash, layer->info.name);
                for (const TerrainEditWeight &channel : found->second.weights)
                {
                    hash = MixBytes(hash, channel.layer);
                    for (const float value : channel.values)
                        hash = MixBits(hash, std::bit_cast<uint32_t>(value));
                }
            }
            return hash;
        }
    }

    void TerrainEditOverlaySession::MarkEdited(TileCoord tile)
    {
        mHeightEdited.insert(tile);
    }

    void TerrainEditOverlaySession::MarkEdited(std::span<const TileCoord> tiles)
    {
        for (const TileCoord &tile : tiles)
            mHeightEdited.insert(tile);
    }

    void TerrainEditOverlaySession::MarkWeightEdited(TileCoord tile)
    {
        mWeightEdited.insert(tile);
    }

    void TerrainEditOverlaySession::MarkWeightEdited(std::span<const TileCoord> tiles)
    {
        for (const TileCoord &tile : tiles)
            mWeightEdited.insert(tile);
    }

    std::shared_ptr<const TerrainOverlay> TerrainEditOverlaySession::Build(
        AssetId asset, std::span<const TerrainEditLayer> layers, const TerrainBaseTileProvider &base,
        std::string &reason)
    {
        return Build(asset, layers, base, TerrainBaseWeightTileProvider{}, {}, {}, 0, reason);
    }

    std::shared_ptr<const TerrainOverlay> TerrainEditOverlaySession::Build(
        AssetId asset, std::span<const TerrainEditLayer> layers, const TerrainBaseTileProvider &base,
        const TerrainBaseWeightTileProvider &weights, std::span<const std::string> baseWeightLayers,
        std::span<const std::string> releaseWeightLayers, uint64_t baseWeightsKey, std::string &reason)
    {
        reason.clear();
        if (asset.IsNull())
        {
            reason = "No terrain asset is selected.";
            return nullptr;
        }
        if (Empty())
        {
            reason = "No tiles have been edited.";
            return nullptr;
        }

        if (!mWeightEdited.empty())
        {
            if (baseWeightLayers.empty())
            {
                reason = "The base weights cache has no material layer table.";
                return nullptr;
            }
            if (baseWeightLayers.size() != releaseWeightLayers.size() ||
                !std::equal(baseWeightLayers.begin(), baseWeightLayers.end(), releaseWeightLayers.begin()))
            {
                reason = "The base weights layer table differs from the release's material layers.";
                return nullptr;
            }
        }

        std::unordered_map<TileCoord, std::shared_ptr<const TileHeights>> tiles;
        tiles.reserve(mHeightEdited.size());
        for (const TileCoord &tile : mHeightEdited)
        {
            bool anyLayerTile = false;
            bool hasHeight = false;
            for (const TerrainEditLayer *layer : TerrainEditCompositingOrder(layers))
            {
                const auto found = layer->tiles.find(tile);
                if (found == layer->tiles.end())
                    continue;
                anyLayerTile = true;
                if (!found->second.values.empty())
                {
                    hasHeight = true;
                    break;
                }
            }
            if (anyLayerTile && !hasHeight)
                continue;

            TerrainBaseHeightsResult<TerrainBaseTile> read = base(tile);
            if (!read || !read.value)
            {
                reason = read.error.message.empty() ? "A base heights tile is missing." : read.error.message;
                return nullptr;
            }

            TerrainResult<TileHeights> composite = CompositeTerrainEditLayers(**read.value, layers);
            if (!composite)
            {
                reason = composite.error.message;
                return nullptr;
            }
            tiles.emplace(tile, std::make_shared<const TileHeights>(std::move(*composite.value)));
        }

        std::unordered_map<TileCoord, std::shared_ptr<const TerrainDecodedWeights>> weightTiles;
        weightTiles.reserve(mWeightEdited.size());
        for (const TileCoord &tile : mWeightEdited)
        {
            const uint64_t hash = TileWeightHash(layers, tile, baseWeightsKey);
            const auto known = mWeightKeys.find(tile);
            const auto payload = mWeightPayloads.find(tile);
            if (known != mWeightKeys.end() && known->second == hash && payload != mWeightPayloads.end())
            {
                weightTiles.emplace(tile, payload->second);
                continue;
            }

            TerrainBaseWeightsResult<TerrainBaseWeightTile> read = weights(tile);
            if (!read || !read.value)
            {
                reason = read.error.message.empty() ? "A base weights tile is missing." : read.error.message;
                return nullptr;
            }

            TerrainResult<TerrainEditWeightComposite> composite = CompositeTerrainEditWeights(**read.value, layers);
            if (!composite)
            {
                reason = composite.error.message;
                return nullptr;
            }

            TerrainResult<TerrainEncodedWeights> encoded =
                EncodeTerrainWeightPayload(composite.value->tile, baseWeightLayers);
            if (!encoded)
            {
                reason = encoded.error.message;
                return nullptr;
            }

            auto decoded = std::make_shared<TerrainDecodedWeights>();
            decoded->tile = tile;
            decoded->intervals = composite.value->tile.intervals;
            decoded->slots = encoded.value->slots;
            for (std::size_t slot = 0; slot < kTerrainWeightSlots; ++slot)
            {
                if (decoded->slots[slot] == 0xFF || decoded->slots[slot] >= baseWeightLayers.size())
                    continue;
                decoded->layers[slot] = baseWeightLayers[decoded->slots[slot]];
            }
            decoded->bytes = std::move(encoded.value->bytes);

            std::shared_ptr<const TerrainDecodedWeights> stored = std::move(decoded);
            mWeightKeys[tile] = hash;
            mWeightPayloads[tile] = stored;
            weightTiles.emplace(tile, std::move(stored));
        }

        for (auto it = mWeightKeys.begin(); it != mWeightKeys.end();)
            it = mWeightEdited.contains(it->first) ? std::next(it) : mWeightKeys.erase(it);
        for (auto it = mWeightPayloads.begin(); it != mWeightPayloads.end();)
            it = mWeightEdited.contains(it->first) ? std::next(it) : mWeightPayloads.erase(it);

        ++mRevision;
        return std::make_shared<const TerrainOverlay>(asset, mRevision, std::move(tiles), std::move(weightTiles));
    }

    void TerrainEditOverlaySession::Clear()
    {
        mHeightEdited.clear();
        mWeightEdited.clear();
        mWeightKeys.clear();
        mWeightPayloads.clear();
        mRevision = 0;
    }

    std::vector<TileCoord> TerrainEditOverlaySession::EditedTiles() const
    {
        return std::vector<TileCoord>(mHeightEdited.begin(), mHeightEdited.end());
    }

    std::vector<TileCoord> TerrainEditOverlaySession::EditedWeightTiles() const
    {
        return std::vector<TileCoord>(mWeightEdited.begin(), mWeightEdited.end());
    }
} // namespace mir
