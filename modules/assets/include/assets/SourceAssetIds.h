/**
 * @file SourceAssetIds.h
 * @author sumin.park
 * @brief The guids of the source files under a set of asset roots, kept in their .meta files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetId.h"
#include "assets/AssetMeta.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mir
{
    struct AssetSourceRoot
    {
        std::filesystem::path physical;
        // the root as it appears in logical paths, for example "games/Game/assets"
        std::string logical;
    };

    class SourceAssetIds
    {
    public:
        explicit SourceAssetIds(std::vector<AssetSourceRoot> roots);

        // Reads every .meta under the roots. Throws std::runtime_error when one is invalid or when
        // two files hold the same guid, naming both. A .meta with no source file is skipped.
        void Scan();

        // The .meta of a source file, written with a new guid when the file has none
        AssetMeta &Meta(const std::filesystem::path &source);

        // The guid of a source file that already has a .meta, or null. Never writes one.
        AssetId KnownId(const std::filesystem::path &source) const;

        // Writes the .meta of a source file after its importer state changed
        void Save(const std::filesystem::path &source);

        // The guid of the file at a logical path such as "games/Game/assets/particles/a.png", or null
        // when no root holds a file there. Writes a .meta for a file that has none.
        AssetId IdAt(std::string_view logicalPath);

    private:
        static std::string Key(const std::filesystem::path &source);
        void Claim(uint64_t guid, const std::string &key);
        std::filesystem::path PhysicalOf(std::string_view logicalPath) const;

        std::vector<AssetSourceRoot> mRoots;
        std::unordered_map<std::string, AssetMeta> mMetas;
        std::unordered_map<uint64_t, std::string> mOwners;
        std::unordered_set<uint64_t> mTaken;
    };
}
