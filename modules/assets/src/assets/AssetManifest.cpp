/**
 * @file AssetManifest.cpp
 * @author Rahul Nair
 * @brief The cooked asset manifest - AssetId -> type/version/path, logical
 *        path and dependency edges, built at cook time and loaded at runtime.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "assets/AssetManifest.h"

#include "assets/AssetBlob.h"
#include "core/fs/FileIO.h"
#include "core/log/Log.h"

#include <algorithm>
#include <cstring>
#include <unordered_set>

namespace mir
{
    namespace
    {
        // entryCount and edgeCount, ahead of the entries
        constexpr std::size_t kHeaderBytes = 2 * sizeof(uint64_t);
    }

    std::vector<std::byte> BuildAssetManifestBlob(std::span<const AssetManifestSourceEntry> entries)
    {
        std::vector<AssetManifestEntry> flatEntries;
        flatEntries.reserve(entries.size());
        std::vector<AssetId> edges;
        std::string pathTable;

        for (const AssetManifestSourceEntry &src : entries)
        {
            AssetManifestEntry entry = src.entry;

            entry.pathOffset = static_cast<uint32_t>(pathTable.size());
            entry.pathLength = static_cast<uint32_t>(src.path.size());
            pathTable.append(src.path);

            entry.logicalPathOffset = static_cast<uint32_t>(pathTable.size());
            entry.logicalPathLength = static_cast<uint32_t>(src.logicalPath.size());
            pathTable.append(src.logicalPath);

            entry.depOffset = static_cast<uint32_t>(edges.size());
            entry.depCount = static_cast<uint32_t>(src.dependencies.size());
            edges.insert(edges.end(), src.dependencies.begin(), src.dependencies.end());

            flatEntries.push_back(entry);
        }

        const uint64_t entryCount = flatEntries.size();
        const uint64_t edgeCount = edges.size();
        const std::size_t entriesBytes = flatEntries.size() * sizeof(AssetManifestEntry);
        const std::size_t edgesBytes = edges.size() * sizeof(AssetId);

        std::vector<std::byte> content(kHeaderBytes + entriesBytes + edgesBytes + pathTable.size());
        std::size_t offset = 0;

        std::memcpy(content.data() + offset, &entryCount, sizeof(entryCount));
        offset += sizeof(entryCount);
        std::memcpy(content.data() + offset, &edgeCount, sizeof(edgeCount));
        offset += sizeof(edgeCount);

        if (!flatEntries.empty())
        {
            std::memcpy(content.data() + offset, flatEntries.data(), entriesBytes);
            offset += entriesBytes;
        }

        if (!edges.empty())
        {
            std::memcpy(content.data() + offset, edges.data(), edgesBytes);
            offset += edgesBytes;
        }

        if (!pathTable.empty())
            std::memcpy(content.data() + offset, pathTable.data(), pathTable.size());

        return BuildAssetBlob(kAssetManifestTypeTag, kAssetManifestContentVersion, content);
    }

    std::optional<AssetManifest> AssetManifest::Parse(std::span<const std::byte> raw)
    {
        const std::optional<AssetBlobView> blob = ParseAssetBlob(raw);
        if (!blob.has_value())
            return std::nullopt;

        if (blob->header.typeTag != kAssetManifestTypeTag)
        {
            MIR_LOG_ERROR("AssetManifest::Parse: type tag mismatch, expected {}, got {}",
                kAssetManifestTypeTag, blob->header.typeTag);
            return std::nullopt;
        }

        // AssetCache::Load checks this for every ordinary asset; the manifest is
        // the one blob that must be trustworthy, so it cannot be the one that
        // skips the check. Without it an old manifest memcpys its stale entry
        // layout into the current struct and yields garbage ids and offsets.
        if (blob->header.contentVersion != kAssetManifestContentVersion)
        {
            MIR_LOG_ERROR("AssetManifest::Parse: unsupported content version {}, expected {}",
                blob->header.contentVersion, kAssetManifestContentVersion);
            return std::nullopt;
        }

        const std::span<const std::byte> content = blob->content;
        if (content.size() < kHeaderBytes)
        {
            MIR_LOG_ERROR("AssetManifest::Parse: content too small for the entry and edge counts");
            return std::nullopt;
        }

        uint64_t entryCount = 0;
        uint64_t edgeCount = 0;
        std::memcpy(&entryCount, content.data(), sizeof(entryCount));
        std::memcpy(&edgeCount, content.data() + sizeof(entryCount), sizeof(edgeCount));

        // Compare against what fits instead of multiplying, so a large count cannot overflow
        const std::size_t availableForEntries = content.size() - kHeaderBytes;
        if (entryCount > availableForEntries / sizeof(AssetManifestEntry))
        {
            MIR_LOG_ERROR("AssetManifest::Parse: content too small for {} entries", entryCount);
            return std::nullopt;
        }
        const std::size_t entriesBytes = static_cast<std::size_t>(entryCount) * sizeof(AssetManifestEntry);

        const std::size_t availableForEdges = availableForEntries - entriesBytes;
        if (edgeCount > availableForEdges / sizeof(AssetId))
        {
            MIR_LOG_ERROR("AssetManifest::Parse: content too small for {} dependency edges", edgeCount);
            return std::nullopt;
        }
        const std::size_t edgesBytes = static_cast<std::size_t>(edgeCount) * sizeof(AssetId);

        AssetManifest manifest;
        manifest.mEntries.resize(entryCount);
        if (entryCount > 0)
            std::memcpy(manifest.mEntries.data(), content.data() + kHeaderBytes, entriesBytes);

        manifest.mEdges.resize(edgeCount);
        if (edgeCount > 0)
            std::memcpy(manifest.mEdges.data(), content.data() + kHeaderBytes + entriesBytes, edgesBytes);

        const std::span<const std::byte> pathBytes = content.subspan(kHeaderBytes + entriesBytes + edgesBytes);
        manifest.mPathTable.assign(reinterpret_cast<const char *>(pathBytes.data()), pathBytes.size());

        manifest.mIndex.reserve(manifest.mEntries.size());
        for (uint32_t i = 0; i < manifest.mEntries.size(); ++i)
        {
            const AssetManifestEntry &entry = manifest.mEntries[i];
            if (static_cast<std::size_t>(entry.pathOffset) + entry.pathLength > manifest.mPathTable.size())
            {
                MIR_LOG_ERROR("AssetManifest::Parse: entry {} path range out of bounds", i);
                return std::nullopt;
            }
            if (static_cast<std::size_t>(entry.logicalPathOffset) + entry.logicalPathLength > manifest.mPathTable.size())
            {
                MIR_LOG_ERROR("AssetManifest::Parse: entry {} logical path range out of bounds", i);
                return std::nullopt;
            }
            if (entry.depOffset > edgeCount || entry.depCount > edgeCount - entry.depOffset)
            {
                MIR_LOG_ERROR("AssetManifest::Parse: entry {} dependency range out of bounds", i);
                return std::nullopt;
            }
            // emplace does not overwrite, so a duplicate would leave the second
            // entry permanently unreachable with no diagnostic - which is exactly
            // what an id collision looks like. Reject instead.
            if (!manifest.mIndex.emplace(entry.id.value, i).second)
            {
                MIR_LOG_ERROR("AssetManifest::Parse: duplicate asset id {:#x} at entry {}", entry.id.value, i);
                return std::nullopt;
            }
            // a name must identify one entry
            if (entry.logicalPathLength > 0)
            {
                const std::string logical = manifest.mPathTable.substr(entry.logicalPathOffset, entry.logicalPathLength);
                if (!manifest.mPathIndex.emplace(logical, i).second)
                {
                    MIR_LOG_ERROR("AssetManifest::Parse: duplicate logical path '{}' at entry {}", logical, i);
                    return std::nullopt;
                }
            }
        }

        // second pass, once every id is indexed
        for (uint32_t i = 0; i < manifest.mEntries.size(); ++i)
        {
            const AssetManifestEntry &entry = manifest.mEntries[i];
            for (const AssetId target : manifest.Dependencies(entry))
                if (!manifest.mIndex.contains(target.value))
                {
                    MIR_LOG_ERROR("AssetManifest::Parse: entry {} depends on {:#x}, which is not in the manifest", i,
                        target.value);
                    return std::nullopt;
                }
        }

        return manifest;
    }

    std::optional<AssetManifest> AssetManifest::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
        if (!bytes.has_value())
            return std::nullopt;

        return Parse(*bytes);
    }

    const AssetManifestEntry *AssetManifest::Find(AssetId id) const
    {
        auto it = mIndex.find(id.value);
        if (it == mIndex.end())
            return nullptr;
        return &mEntries[it->second];
    }

    const AssetManifestEntry *AssetManifest::FindByPath(std::string_view logicalPath) const
    {
        auto it = mPathIndex.find(std::string(logicalPath));
        if (it == mPathIndex.end())
            return nullptr;
        return &mEntries[it->second];
    }

    std::string_view AssetManifest::PathOf(const AssetManifestEntry &entry) const
    {
        return std::string_view(mPathTable).substr(entry.pathOffset, entry.pathLength);
    }

    std::string_view AssetManifest::LogicalPathOf(const AssetManifestEntry &entry) const
    {
        return std::string_view(mPathTable).substr(entry.logicalPathOffset, entry.logicalPathLength);
    }

    std::span<const AssetId> AssetManifest::Dependencies(const AssetManifestEntry &entry) const
    {
        return std::span<const AssetId>(mEdges).subspan(entry.depOffset, entry.depCount);
    }

    uint64_t AssetManifest::ContentHash(const AssetManifestEntry &entry) const
    {
        const std::string_view path = PathOf(entry);
        const size_t dash = path.rfind('-'), dot = path.rfind('.');
        if (dash == std::string_view::npos || dot == std::string_view::npos || dot <= dash + 1 || dot - dash - 1 != 16)
            return 0;
        uint64_t value = 0;
        for (size_t i = dash + 1; i < dot; ++i)
        {
            const char c = path[i];
            uint64_t digit;
            if (c >= '0' && c <= '9')
                digit = c - '0';
            else if (c >= 'a' && c <= 'f')
                digit = c - 'a' + 10;
            else
                return 0;
            value = (value << 4) | digit;
        }
        return value;
    }

    namespace
    {
        constexpr uint64_t Mix(uint64_t hash, uint64_t value)
        {
            hash = (hash ^ value) * 0x100000001b3ull;
            return hash ^ (hash >> 29);
        }
    }

    namespace
    {
        // Known content never sets the top bit, so a caller can use that bit for a value of its own.
        constexpr uint64_t Known(uint64_t hash)
        {
            hash &= 0x7FFFFFFFFFFFFFFFull;
            return hash == 0 ? 1 : hash;
        }
    }

    uint64_t AssetManifest::EntryContent(AssetId id) const
    {
        const AssetManifestEntry *entry = Find(id);
        const uint64_t content = entry != nullptr ? ContentHash(*entry) : 0;
        if (content == 0)
            return 0;
        uint64_t hash = Mix(0xcbf29ce484222325ull, id.value);
        hash = Mix(Mix(Mix(hash, entry->typeTag), entry->contentVersion), content);
        return Known(hash);
    }

    uint64_t AssetManifest::ClosureContent(AssetId root, std::span<const uint32_t> tags) const
    {
        const std::vector<AssetId> closure = Closure(root, tags);
        if (closure.empty())
            return 0;
        uint64_t hash = 0xcbf29ce484222325ull;
        for (const AssetId id : closure)
        {
            const uint64_t content = EntryContent(id);
            if (content == 0)
                return 0;
            hash = Mix(hash, content);
        }
        return Known(hash);
    }

    std::vector<AssetId> AssetManifest::Closure(AssetId root, std::span<const uint32_t> tags) const
    {
        std::vector<AssetId> result;
        if (Find(root) == nullptr)
            return result;

        // result doubles as the queue, so the walk is breadth first
        std::unordered_set<uint64_t> seen{ root.value };
        result.push_back(root);
        for (std::size_t next = 0; next < result.size(); ++next)
        {
            const AssetManifestEntry *entry = Find(result[next]);
            for (const AssetId dependency : Dependencies(*entry))
            {
                if (!seen.insert(dependency.value).second)
                    continue;

                if (!tags.empty())
                {
                    const AssetManifestEntry *target = Find(dependency);
                    if (target == nullptr || std::find(tags.begin(), tags.end(), target->typeTag) == tags.end())
                        continue;
                }
                result.push_back(dependency);
            }
        }
        return result;
    }
}
