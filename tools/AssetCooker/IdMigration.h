/**
 * @file IdMigration.h
 * @author sumin.park
 * @brief One-off move of model ids from the old import.json files to .meta guids.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/SourceAssetIds.h>

#include <filesystem>
#include <vector>

namespace mir::cook
{
    struct IdMigrationOptions
    {
        std::vector<AssetSourceRoot> roots;
        // folders of JSON that store model ids, such as scenes and prefabs
        std::vector<std::filesystem::path> dataDirs;
        // false only reports what would change
        bool apply = false;
    };

    // Gives every glTF with an .import.json a .meta and a new guid, carries its part keys over,
    // rewrites the old ids stored under dataDirs to the new ones, and deletes the .import.json.
    // Throws std::runtime_error when an .import.json cannot be read.
    void MigrateIds(const IdMigrationOptions &options);
}
