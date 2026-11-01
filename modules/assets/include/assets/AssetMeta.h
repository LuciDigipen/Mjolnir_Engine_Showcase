/**
 * @file AssetMeta.h
 * @author sumin.park
 * @brief The .meta file kept next to a source asset, which holds its guid.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetId.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace mir
{
    inline constexpr uint32_t kAssetMetaVersion = 1;
    inline constexpr std::string_view kAssetMetaExtension = ".meta";

    struct AssetMeta
    {
        // identifies the asset, and stays the same when its file is renamed or moved
        AssetId guid;
        // the importer's own state as a JSON object
        std::string importer = "{}";
    };

    // The id of something inside a package, from the package's guid and a key that names the part
    AssetId MakeChildAssetId(AssetId parent, std::string_view key);

    // The .meta file of a source file: its path with .meta added
    std::filesystem::path AssetMetaPath(const std::filesystem::path &source);

    // A random guid, never null and never one in taken
    AssetId MakeRandomGuid(const std::unordered_set<uint64_t> &taken = {});

    // Null when the file is missing, or logged and null when it is not a valid .meta of this version
    std::optional<AssetMeta> ReadAssetMeta(const std::filesystem::path &metaPath);
    bool WriteAssetMeta(const std::filesystem::path &metaPath, const AssetMeta &meta);

    // Moves the .meta of a source file along with it. Does nothing when there is none.
    void MoveAssetMeta(const std::filesystem::path &from, const std::filesystem::path &to);
    void RemoveAssetMeta(const std::filesystem::path &source);

    // A file beside the assets that is not an asset itself: a .meta, an old .import.json, a
    // temporary .pending or a .gitkeep. Scans of source assets skip these.
    bool IsAssetToolingFile(const std::filesystem::path &path);
}
