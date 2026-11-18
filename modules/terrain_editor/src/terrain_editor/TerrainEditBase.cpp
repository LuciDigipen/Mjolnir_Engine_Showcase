/**
 * @file TerrainEditBase.cpp
 * @author Jackey Cen
 * @brief The base heights cache the brush edits against, and whether brushing is allowed.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainEditBase.h>

#include <terrain/TerrainRelease.h>

#include <core/fs/FileIO.h>

#include <format>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr std::size_t kPointerProbeBytes = 64;

        std::optional<std::string> PeekPointer(const std::filesystem::path &releasesRoot, AssetId asset)
        {
            const std::optional<std::vector<std::byte>> bytes =
                ReadFirstBytes(TerrainBaseHeightsPointerPath(releasesRoot, asset), kPointerProbeBytes);
            if (!bytes)
                return std::nullopt;
            return std::string(reinterpret_cast<const char *>(bytes->data()), bytes->size());
        }
    }

    bool TerrainBaseCache::Ensure(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        if (asset.IsNull())
        {
            Clear();
            mError = "No terrain asset is selected.";
            return false;
        }

        const std::optional<std::string> pointer = PeekPointer(releasesRoot, asset);
        if (mTried && mAsset == asset && pointer == mPointerText)
            return mBase.has_value();
        return Open(releasesRoot, asset);
    }

    void TerrainBaseCache::Clear()
    {
        mBase.reset();
        mAsset = kNullAssetId;
        mError.clear();
        mPointerText.reset();
        mTried = false;
        mTiles.clear();
    }

    bool TerrainBaseCache::Open(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        mAsset = asset;
        mTried = true;
        mPointerText = PeekPointer(releasesRoot, asset);
        mBase.reset();
        mError.clear();
        mTiles.clear();

        TerrainBaseHeightsResult<TerrainBaseHeights> opened = TerrainBaseHeights::Open(releasesRoot, asset);
        if (!opened)
        {
            mError = opened.error.message;
            return false;
        }
        mBase.emplace(std::move(*opened.value));
        return true;
    }

    uint64_t TerrainBaseCache::KeyValue() const
    {
        return mBase ? mBase->Key().value : 0;
    }

    std::optional<TerrainLayout> TerrainBaseCache::ReferenceLayout() const    {
        if (!mBase)
            return std::nullopt;
        return mBase->Release().Layout();
    }

    std::optional<TerrainLayout> TerrainBaseCache::Layout(TileCoord tile) const
    {
        if (!mBase)
            return std::nullopt;
        const TerrainTileRecord *record = mBase->Release().Index().Find(tile);
        if (record == nullptr || !record->source || record->samplesPerSide < 2)
            return std::nullopt;
        return TerrainLayout::Create(record->sampleSpacing, record->samplesPerSide - 1,
                                     mBase->Release().Layout().Bounds());
    }

    TerrainBaseHeightsResult<TerrainBaseTile> TerrainBaseCache::ReadTile(TileCoord tile)
    {
        TerrainBaseHeightsResult<TerrainBaseTile> result;
        if (!mBase)
        {
            result.error = { TerrainBaseHeightsErrorCode::InvalidInput,
                mError.empty() ? "No base heights cache." : mError };
            return result;
        }

        const auto found = mTiles.find(tile);
        if (found != mTiles.end())
        {
            result.value = found->second;
            return result;
        }

        TerrainBaseHeightsResult<TileHeights> read = mBase->ReadTile(tile);
        if (!read)
        {
            result.error = read.error;
            return result;
        }

        TerrainBaseTile loaded = std::make_shared<const TileHeights>(std::move(*read.value));
        mTiles.emplace(tile, loaded);
        result.value = std::move(loaded);
        return result;
    }

    TerrainBaseTileProvider TerrainBaseCache::Provider()
    {
        return [this](TileCoord tile) { return ReadTile(tile); };
    }

    bool TerrainBaseWeightsCache::Ensure(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        if (asset.IsNull())
        {
            Clear();
            mError = "No terrain asset is selected.";
            return false;
        }

        const std::optional<std::string> pointer = PeekPointer(releasesRoot, asset);
        if (mTried && mAsset == asset && pointer == mPointerText)
            return mBase.has_value();
        return Open(releasesRoot, asset);
    }

    void TerrainBaseWeightsCache::Clear()
    {
        mBase.reset();
        mAsset = kNullAssetId;
        mError.clear();
        mPointerText.reset();
        mTried = false;
        mTiles.clear();
    }

    bool TerrainBaseWeightsCache::Open(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        mAsset = asset;
        mTried = true;
        mPointerText = PeekPointer(releasesRoot, asset);
        mBase.reset();
        mError.clear();
        mTiles.clear();

        TerrainBaseWeightsResult<TerrainBaseWeights> opened = TerrainBaseWeights::Open(releasesRoot, asset);
        if (!opened)
        {
            mError = opened.error.message;
            return false;
        }
        mBase.emplace(std::move(*opened.value));
        return true;
    }

    uint64_t TerrainBaseWeightsCache::KeyValue() const
    {
        return mBase ? mBase->Key().value : 0;
    }

    std::span<const std::string> TerrainBaseWeightsCache::Layers() const
    {
        return mBase ? mBase->Layers() : std::span<const std::string>{};
    }

    std::optional<TerrainLayout> TerrainBaseWeightsCache::Layout() const
    {
        if (!mBase)
            return std::nullopt;
        return mBase->Layout();
    }

    TerrainBaseWeightsResult<TerrainBaseWeightTile> TerrainBaseWeightsCache::ReadTile(TileCoord tile)
    {
        TerrainBaseWeightsResult<TerrainBaseWeightTile> result;
        if (!mBase)
        {
            result.error = { TerrainBaseWeightsErrorCode::InvalidInput,
                mError.empty() ? "No base weights cache." : mError };
            return result;
        }

        const auto found = mTiles.find(tile);
        if (found != mTiles.end())
        {
            result.value = found->second;
            return result;
        }

        TerrainBaseWeightsResult<TerrainWeightTile> read = mBase->ReadTile(tile);
        if (!read)
        {
            result.error = read.error;
            return result;
        }

        TerrainBaseWeightTile loaded = std::make_shared<const TerrainWeightTile>(std::move(*read.value));
        mTiles.emplace(tile, loaded);
        result.value = std::move(loaded);
        return result;
    }

    TerrainBaseWeightTileProvider TerrainBaseWeightsCache::Provider()
    {
        return [this](TileCoord tile) { return ReadTile(tile); };
    }

    TerrainBrushAvailability EvaluateTerrainBrushAvailability(bool active, bool editing, bool sceneDocument,
                                                              bool documentEditable, bool hasLayer, bool baseReady,
                                                              std::string_view baseError)
    {
        TerrainBrushAvailability availability;
        if (!active)
            return availability;
        if (!editing)
        {
            availability.message = "Terrain brushes work in Edit.";
            return availability;
        }
        if (!sceneDocument)
        {
            availability.message = "Terrain brushes paint the scene, not a Prefab.";
            return availability;
        }
        if (!documentEditable)
        {
            availability.message = "The Edit Layers cannot be edited.";
            return availability;
        }
        if (!hasLayer)
        {
            availability.message = "Select an Edit Layer to brush.";
            return availability;
        }
        if (!baseReady)
        {
            constexpr std::string_view kBuildHint =
                "Build the game so TerrainTool sync writes the base heights cache.";
            availability.message = baseError.empty() ? std::string(kBuildHint)
                                                     : std::format("{} {}", baseError, kBuildHint);
            return availability;
        }

        availability.enabled = true;
        availability.message = "Hold Ctrl to lower.";
        return availability;
    }

    TerrainBrushAvailability EvaluateTerrainPaintAvailability(bool active, bool editing, bool sceneDocument,
                                                              bool documentEditable, bool hasLayer, bool weightsReady,
                                                              std::string_view weightsError, bool hasMaterialLayer,
                                                              bool recipeHasMaterials)
    {
        TerrainBrushAvailability availability;
        if (!active)
            return availability;
        if (!editing)
        {
            availability.message = "Terrain brushes work in Edit.";
            return availability;
        }
        if (!sceneDocument)
        {
            availability.message = "Terrain brushes paint the scene, not a Prefab.";
            return availability;
        }
        if (!documentEditable)
        {
            availability.message = "The Edit Layers cannot be edited.";
            return availability;
        }
        if (!hasLayer)
        {
            availability.message = "Select an Edit Layer to brush.";
            return availability;
        }
        if (!weightsReady)
        {
            if (!recipeHasMaterials)
            {
                availability.message =
                    "This terrain has no material layers yet; add them in the Material layers section.";
                return availability;
            }
            constexpr std::string_view kBuildHint =
                "Build the game to create the material weights cache.";
            availability.message = weightsError.empty() ? std::string(kBuildHint)
                                                        : std::format("{} {}", weightsError, kBuildHint);
            return availability;
        }
        if (!hasMaterialLayer)
        {
            availability.message = "Choose a material layer to paint.";
            return availability;
        }

        availability.enabled = true;
        availability.message = "Hold Ctrl to erase.";
        return availability;
    }
} // namespace mir
