/**
 * @file TerrainBaseCache.cpp
 * @author Jackey Cen
 * @brief Writes the generator's source heights as a local cache beside an asset's releases.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainBaseCache.h"

#include "TerrainReport.h"

#include <core/fs/FileIO.h>
#include <terrain/TerrainBaseHeights.h>
#include <terrain/TerrainRelease.h>

#include <algorithm>
#include <format>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace mir::terrainbake
{
    namespace
    {
        namespace fs = std::filesystem;

        bool EndsWith(std::string_view text, std::string_view tail)
        {
            return text.size() >= tail.size() && text.substr(text.size() - tail.size()) == tail;
        }
    }

    TerrainSourceFingerprint BaseHeightsFingerprint(const TerrainRecipe &recipe)
    {
        TerrainRecipe unedited = recipe;
        unedited.editStack.reset();
        unedited.editStackHash = 0;
        return FingerprintRecipe(unedited);
    }

    std::optional<TerrainEncodedRelease> EncodeBaseHeights(const TerrainRecipe &recipe, AssetId asset,
                                                          TerrainSourceFingerprint key,
                                                          std::span<const TileHeights> tiles, std::string &error)
    {
        if (key.IsNull())
        {
            error = "no base key";
            return std::nullopt;
        }
        if (tiles.empty())
        {
            error = "no source tiles";
            return std::nullopt;
        }
        const std::vector<TerrainLayout> layers = recipe.SourceLayers();
        std::vector<TerrainTileWrite> writes;
        writes.reserve(tiles.size());
        for (const TileHeights &tile : tiles)
        {
            const std::optional<double> spacing = recipe.SourceSpacing(tile.Tile());
            const auto layer = spacing ? std::find_if(layers.begin(), layers.end(),
                                                      [&](const TerrainLayout &candidate)
                                                      {
                                                          return candidate.SampleSpacing() == *spacing;
                                                      })
                                       : layers.end();
            if (!spacing || layer == layers.end())
            {
                error = std::format("tile ({}, {}) has no source layer", tile.Tile().x, tile.Tile().z);
                return std::nullopt;
            }
            writes.push_back({&tile, static_cast<uint32_t>(layer - layers.begin()), true, std::nullopt});
        }

        const TerrainReleaseInfo info{asset, key, kTerrainFormatVersion, TerrainReleaseId(key.value)};
        TerrainResult<TerrainEncodedRelease> encoded = EncodeTerrainRelease(info, layers, writes);
        if (!encoded)
        {
            error = encoded.error.message;
            return std::nullopt;
        }
        return std::move(*encoded.value);
    }

    bool BaseHeightsCurrent(const std::filesystem::path &releasesRoot, AssetId asset, TerrainSourceFingerprint key)
    {
        const std::optional<std::string> text = ReadText(TerrainBaseHeightsPointerPath(releasesRoot, asset));
        if (!text)
            return false;
        std::string_view name = *text;
        if (name.ends_with("\r\n"))
            name.remove_suffix(2);
        else if (name.ends_with('\n'))
            name.remove_suffix(1);
        const std::optional<uint64_t> named = ParseTerrainIdHex(name);
        std::error_code ec;
        return named && *named == key.value &&
               fs::is_regular_file(TerrainBaseHeightsDir(releasesRoot, asset, key) / kTerrainIndexFileName, ec);
    }

    bool WriteBaseHeights(const std::filesystem::path &releasesRoot, AssetId asset,
                          const TerrainBaseHeightsEncoding &base, std::string &error)
    {
        if (base.key.IsNull() || !base.encoded)
        {
            error = base.error.empty() ? "no base encoding" : base.error;
            return false;
        }

        const fs::path root = TerrainBaseHeightsRoot(releasesRoot, asset);
        const std::string hex = TerrainIdHex(base.key.value);
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec)
        {
            error = std::format("cannot create {} ({})", PathUtf8(root), ec.message());
            return false;
        }
        // Under the publish lock every .partial and pointer temp is left from a dead writer.
        for (const fs::directory_entry &entry : fs::directory_iterator(root, ec))
        {
            const std::string name = PathUtf8(entry.path().filename());
            std::error_code ignored;
            if ((entry.is_directory(ignored) && EndsWith(name, ".partial")) || name == "current.tmp")
                fs::remove_all(entry.path(), ignored);
        }

        const fs::path staging = root / (hex + ".partial");
        fs::remove_all(staging, ec);
        if (!fs::create_directory(staging, ec))
        {
            error = std::format("cannot create {} ({})", PathUtf8(staging),
                                ec ? ec.message() : "it exists");
            return false;
        }
        auto abandon = [&](std::string why)
        {
            std::error_code ignored;
            fs::remove_all(staging, ignored);
            error = std::move(why);
            return false;
        };
        if (!WriteBytes(staging / kTerrainIndexFileName, base.encoded->index))
            return abandon(std::format("cannot write {}", PathUtf8(staging / kTerrainIndexFileName)));
        for (const TerrainEncodedPack &pack : base.encoded->packs)
            if (!WriteBytes(staging / pack.fileName, pack.bytes))
                return abandon(std::format("cannot write {}", PathUtf8(staging / pack.fileName)));

        const std::shared_ptr<TerrainFileSource> source = std::make_shared<TerrainFileSource>(staging);
        TerrainResult<TerrainReleaseHandle> opened = TerrainReleaseHandle::Open(source);
        if (!opened)
            return abandon(std::format("cannot read back {}: {}", PathUtf8(staging), opened.error.message));
        const TerrainPackError verified = opened.value->VerifyAllPacks();
        if (verified.code != TerrainPackErrorCode::None)
            return abandon(std::format("cannot read back {}: {}", PathUtf8(staging), verified.message));
        const TerrainReleaseInfo &info = opened.value->Info();
        if (info.asset != asset || info.source != base.key || info.release.value != base.key.value)
            return abandon(std::format("{} was written with another asset, key or release", PathUtf8(staging)));

        const fs::path finalDir = TerrainBaseHeightsDir(releasesRoot, asset, base.key);
        if (fs::exists(finalDir, ec))
        {
            fs::remove_all(finalDir, ec);
            if (ec)
                return abandon(std::format("cannot remove the old cache {} ({})", PathUtf8(finalDir), ec.message()));
        }
        fs::rename(staging, finalDir, ec);
        if (ec)
            return abandon(std::format("cannot rename {} to {} ({})", PathUtf8(staging), PathUtf8(finalDir),
                                       ec.message()));

        const fs::path pointer = TerrainBaseHeightsPointerPath(releasesRoot, asset);
        if (!WriteTextAtomic(pointer, hex))
            return abandon(std::format("cannot replace {}", PathUtf8(pointer)));

        for (const fs::directory_entry &entry : fs::directory_iterator(root, ec))
        {
            const std::string name = PathUtf8(entry.path().filename());
            const std::optional<uint64_t> id = ParseTerrainIdHex(name);
            std::error_code isDir;
            if (!id || name == hex || !entry.is_directory(isDir))
                continue;
            std::error_code ignored;
            fs::remove_all(entry.path(), ignored);
        }
        return true;
    }
}
