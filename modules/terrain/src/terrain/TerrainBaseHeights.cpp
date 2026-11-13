/**
 * @file TerrainBaseHeights.cpp
 * @author Jackey Cen
 * @brief Opens the generator's cached source heights kept beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "terrain/TerrainBaseHeights.h"

#include <core/fs/FileIO.h>

#include <format>
#include <memory>
#include <utility>

namespace mir
{
    namespace
    {
        using Code = TerrainBaseHeightsErrorCode;

        Code MapPackCode(TerrainPackErrorCode code)
        {
            switch (code)
            {
            case TerrainPackErrorCode::None:
                return Code::None;
            case TerrainPackErrorCode::Io:
                return Code::Io;
            case TerrainPackErrorCode::Unsupported:
                return Code::Unsupported;
            case TerrainPackErrorCode::InvalidInput:
                return Code::InvalidInput;
            default:
                return Code::Corrupt;
            }
        }
    }

    std::filesystem::path TerrainBaseHeightsRoot(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        return TerrainAssetReleasesDir(releasesRoot, asset) / kTerrainBaseHeightsDirName;
    }

    std::filesystem::path TerrainBaseHeightsDir(const std::filesystem::path &releasesRoot, AssetId asset,
                                                TerrainSourceFingerprint key)
    {
        return TerrainBaseHeightsRoot(releasesRoot, asset) / TerrainIdHex(key.value);
    }

    std::filesystem::path TerrainBaseHeightsPointerPath(const std::filesystem::path &releasesRoot, AssetId asset)
    {
        return TerrainBaseHeightsRoot(releasesRoot, asset) / kTerrainCurrentPointerFileName;
    }

    TerrainBaseHeights::TerrainBaseHeights(TerrainSourceFingerprint key, TerrainReleaseHandle release)
        : mKey(key), mRelease(std::move(release))
    {
    }

    TerrainBaseHeightsResult<TerrainBaseHeights> TerrainBaseHeights::Open(const std::filesystem::path &releasesRoot,
                                                                          AssetId asset)
    {
        TerrainBaseHeightsResult<TerrainBaseHeights> result;
        if (asset.IsNull())
        {
            result.error = {Code::InvalidInput, "no asset id"};
            return result;
        }

        const std::filesystem::path pointer = TerrainBaseHeightsPointerPath(releasesRoot, asset);
        const std::optional<std::string> text = ReadText(pointer);
        if (!text)
        {
            result.error = {Code::MissingPointer,
                            std::format("{}: no base heights cache (run TerrainTool sync)", pointer.generic_string())};
            return result;
        }
        const TerrainResult<TerrainReleaseId> parsed = ParseTerrainCurrentPointer(*text);
        if (!parsed)
        {
            result.error = {Code::Corrupt,
                            std::format("{}: {}", pointer.generic_string(), parsed.error.message)};
            return result;
        }
        const TerrainSourceFingerprint key(parsed.value->value);
        const std::filesystem::path dir = TerrainBaseHeightsDir(releasesRoot, asset, key);
        const std::shared_ptr<TerrainFileSource> source = std::make_shared<TerrainFileSource>(dir);
        TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(source);
        if (!opened)
        {
            result.error = {MapPackCode(opened.error.code),
                            std::format("{}: {}", dir.generic_string(), opened.error.message)};
            return result;
        }

        const TerrainIndex &index = opened.value->Index();
        auto corrupt = [&result](std::string why)
        {
            result.error = {Code::Corrupt, std::move(why)};
            return result;
        };
        if (index.release.asset != asset || index.release.source != key ||
            index.release.release.value != key.value)
            return corrupt(std::format("{}: the index is asset {} release {} of source {}, not this asset's base {}",
                                       dir.generic_string(), TerrainIdHex(index.release.asset.value),
                                       TerrainIdHex(index.release.release.value),
                                       TerrainIdHex(index.release.source.value), TerrainIdHex(key.value)));
        if (index.layers.empty() || index.tiles.empty())
            return corrupt(std::format("{}: the base cache has no source layers or tiles", dir.generic_string()));
        for (const TerrainTileRecord &record : index.tiles)
            if (!record.source || record.kind != TerrainPayloadKind::Height)
                return corrupt(std::format("{}: tile ({}, {}) is not a source height", dir.generic_string(),
                                           record.tile.x, record.tile.z));
        if (!index.regions.empty())
            return corrupt(std::format("{}: the base cache has derived Regions", dir.generic_string()));

        result.value = TerrainBaseHeights(key, std::move(*opened.value));
        return result;
    }

    TerrainBaseHeightsResult<TileHeights> TerrainBaseHeights::ReadTile(TileCoord tile) const
    {
        TerrainBaseHeightsResult<TileHeights> result;
        TerrainResult<TileHeights> read = mRelease.ReadTile({mRelease.Info().asset, tile});
        if (!read)
        {
            const Code code = read.error.code == TerrainPackErrorCode::NotFound ? Code::MissingTile
                                                                               : MapPackCode(read.error.code);
            result.error = {code, read.error.message};
            return result;
        }
        result.value = std::move(*read.value);
        return result;
    }

    std::string_view DescribeTerrainBaseHeightsError(TerrainBaseHeightsErrorCode code)
    {
        switch (code)
        {
        case TerrainBaseHeightsErrorCode::None:
            return "ok";
        case TerrainBaseHeightsErrorCode::MissingPointer:
            return "no pointer";
        case TerrainBaseHeightsErrorCode::MissingTile:
            return "no such tile";
        case TerrainBaseHeightsErrorCode::Corrupt:
            return "corrupt";
        case TerrainBaseHeightsErrorCode::Unsupported:
            return "unsupported";
        case TerrainBaseHeightsErrorCode::Io:
            return "io";
        case TerrainBaseHeightsErrorCode::InvalidInput:
            return "invalid input";
        }
        return "?";
    }
}
