/**
 * @file SourceAssetIds.cpp
 * @author sumin.park
 * @brief The guids of the source files under a set of asset roots, kept in their .meta files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "assets/SourceAssetIds.h"

#include <core/log/Log.h>

#include <format>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        bool IsMetaFile(const std::filesystem::path &path)
        {
            return path.extension().string() == kAssetMetaExtension;
        }
    }

    SourceAssetIds::SourceAssetIds(std::vector<AssetSourceRoot> roots) : mRoots(std::move(roots)) {}

    std::string SourceAssetIds::Key(const std::filesystem::path &source)
    {
        return std::filesystem::weakly_canonical(source).generic_string();
    }

    void SourceAssetIds::Claim(uint64_t guid, const std::string &key)
    {
        const auto [owner, inserted] = mOwners.emplace(guid, key);
        if (inserted || owner->second == key)
            return;

        // the first file is gone, so the .meta moved with its file
        std::error_code ec;
        if (!std::filesystem::exists(owner->second, ec))
        {
            mMetas.erase(owner->second);
            owner->second = key;
            return;
        }
        throw std::runtime_error(std::format(
            "{:016x} is the guid of both '{}' and '{}'; delete the .meta of the copy", guid, owner->second, key));
    }

    void SourceAssetIds::Scan()
    {
        for (const AssetSourceRoot &root : mRoots)
        {
            std::error_code ec;
            for (const auto &entry : std::filesystem::recursive_directory_iterator(root.physical, ec))
            {
                if (!entry.is_regular_file() || !IsMetaFile(entry.path()))
                    continue;

                std::filesystem::path source = entry.path();
                source.replace_extension();
                if (!std::filesystem::exists(source))
                {
                    MIR_LOG_WARN("{} has no source file, so it is ignored", entry.path().string());
                    continue;
                }

                std::optional<AssetMeta> meta = ReadAssetMeta(entry.path());
                if (!meta)
                    throw std::runtime_error(entry.path().string() + " is not a valid .meta file");

                const std::string key = Key(source);
                Claim(meta->guid.value, key);
                mTaken.insert(meta->guid.value);
                mMetas.insert_or_assign(key, std::move(*meta));
            }
        }
    }

    AssetMeta &SourceAssetIds::Meta(const std::filesystem::path &source)
    {
        const std::string key = Key(source);
        if (const auto found = mMetas.find(key); found != mMetas.end())
            return found->second;

        // not seen by Scan, so it is on disk and unread, or it has no .meta yet
        std::optional<AssetMeta> meta = ReadAssetMeta(AssetMetaPath(source));
        if (!meta)
        {
            meta = AssetMeta{};
            meta->guid = MakeRandomGuid(mTaken);
            if (!WriteAssetMeta(AssetMetaPath(source), *meta))
                throw std::runtime_error("cannot write " + AssetMetaPath(source).string());
        }
        else
            Claim(meta->guid.value, key);

        mOwners.emplace(meta->guid.value, key);
        mTaken.insert(meta->guid.value);
        return mMetas.emplace(key, std::move(*meta)).first->second;
    }

    AssetId SourceAssetIds::KnownId(const std::filesystem::path &source) const
    {
        if (const auto found = mMetas.find(Key(source)); found != mMetas.end())
            return found->second.guid;
        const std::optional<AssetMeta> meta = ReadAssetMeta(AssetMetaPath(source));
        return meta ? meta->guid : kNullAssetId;
    }

    void SourceAssetIds::Save(const std::filesystem::path &source)
    {
        if (!WriteAssetMeta(AssetMetaPath(source), Meta(source)))
            throw std::runtime_error("cannot write " + AssetMetaPath(source).string());
    }

    std::filesystem::path SourceAssetIds::PhysicalOf(std::string_view logicalPath) const
    {
        for (const AssetSourceRoot &root : mRoots)
        {
            if (logicalPath.size() <= root.logical.size() + 1 || logicalPath.compare(0, root.logical.size(), root.logical) != 0 ||
                logicalPath[root.logical.size()] != '/')
                continue;
            const std::filesystem::path physical = root.physical / std::string(logicalPath.substr(root.logical.size() + 1));
            std::error_code ec;
            if (std::filesystem::is_regular_file(physical, ec))
                return physical;
        }
        return {};
    }

    AssetId SourceAssetIds::IdAt(std::string_view logicalPath)
    {
        const std::filesystem::path physical = PhysicalOf(logicalPath);
        return physical.empty() ? kNullAssetId : Meta(physical).guid;
    }
}
