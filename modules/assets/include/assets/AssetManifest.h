/**
 * @file AssetManifest.h
 * @author Rahul Nair
 * @brief The cooked asset manifest - AssetId -> type/version/path, built at
 *        cook time and loaded at runtime.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "assets/AssetId.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace mir
{
    inline constexpr std::string_view kAssetManifestFileName = "manifest.blob";

    inline constexpr uint32_t kAssetManifestTypeTag = 0x4D414E46u;

    inline constexpr uint32_t kAssetManifestContentVersion = 2u;

    // Offsets index the path table, depOffset indexes the edge table
    struct AssetManifestEntry
    {
        AssetId id{};
        uint32_t typeTag = 0;
        uint32_t contentVersion = 0;
        // cooked blob file
        uint32_t pathOffset = 0;
        uint32_t pathLength = 0;
        // readable name, empty when the asset has none
        uint32_t logicalPathOffset = 0;
        uint32_t logicalPathLength = 0;
        // direct dependencies only
        uint32_t depOffset = 0;
        uint32_t depCount = 0;
    };

    static_assert(std::is_trivially_copyable_v<AssetManifestEntry>);
    static_assert(std::is_standard_layout_v<AssetManifestEntry>);
    static_assert(std::is_trivially_copyable_v<AssetId>);
    static_assert(sizeof(AssetManifestEntry) == 40);

    // The offsets and counts in entry are set by BuildAssetManifestBlob
    struct AssetManifestSourceEntry
    {
        AssetManifestEntry entry;
        std::string_view path;
        std::string_view logicalPath;
        std::span<const AssetId> dependencies;
    };

    std::vector<std::byte> BuildAssetManifestBlob(std::span<const AssetManifestSourceEntry> entries);

    class AssetManifest
    {
    public:
        static std::optional<AssetManifest> Parse(std::span<const std::byte> raw);
        static std::optional<AssetManifest> LoadFile(const std::filesystem::path &path);

        const AssetManifestEntry *Find(AssetId id) const;
        // The entry with this logical path, or null
        const AssetManifestEntry *FindByPath(std::string_view logicalPath) const;
        std::string_view PathOf(const AssetManifestEntry &entry) const;
        std::string_view LogicalPathOf(const AssetManifestEntry &entry) const;
        // The assets this entry depends on directly, each one an entry of this manifest
        std::span<const AssetId> Dependencies(const AssetManifestEntry &entry) const;

        // Root first, then what it reaches, each asset once. With tags, only entries
        // of those types are kept and the walk continues only through kept entries.
        // The root is always kept, and an unknown root gives an empty result.
        std::vector<AssetId> Closure(AssetId root, std::span<const uint32_t> tags = {}) const;

        // The blob's content hash, read from its content-addressed file name (<id>-<hash>.blob). Zero when the
        // name carries none, so the content is unknown and must not be assumed unchanged.
        uint64_t ContentHash(const AssetManifestEntry &entry) const;
        // One value for an entry alone: its id, type, version and blob hash. Zero when unknown or absent. Known
        // content values never have the top bit set.
        uint64_t EntryContent(AssetId id) const;
        // One value for the root and everything Closure reaches under tags, in closure order. Equal values mean
        // equal decoded results. Zero when any member is unknown or the root is not in the manifest. Empty tags
        // follow Closure and reach every dependency, use EntryContent for a root-only decode.
        uint64_t ClosureContent(AssetId root, std::span<const uint32_t> tags = {}) const;

        std::size_t Count() const { return mEntries.size(); }
        std::span<const AssetManifestEntry> Entries() const { return mEntries; }

    private:
        std::vector<AssetManifestEntry> mEntries;
        std::vector<AssetId> mEdges;
        std::string mPathTable;
        std::unordered_map<uint64_t, uint32_t> mIndex;
        // owns its keys, a view into mPathTable would dangle after a move
        std::unordered_map<std::string, uint32_t> mPathIndex;
    };
} // namespace mir
