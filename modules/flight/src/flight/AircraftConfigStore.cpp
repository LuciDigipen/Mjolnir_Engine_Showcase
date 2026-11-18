/**
 * @file AircraftConfigStore.cpp
 * @author Jackey Cen
 * @brief Decodes each aircraft configuration path once, keeps the result and tracks unsaved edits.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight/AircraftConfigStore.h>

#include <flight/AircraftConfig.h>

#include <assets/AssetBlob.h>
#include <assets/AssetCache.h>
#include <assets/AssetResolver.h>
#include <core/log/Log.h>

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace mir
{
    AircraftConfigStore::AircraftConfigStore(Source source) : mSource(std::move(source)) {}

    bool AircraftConfigStore::Decode(const Source &source, std::string_view path, AircraftConfig &config,
        std::string &error) const
    {
        const AssetBlobView *blob = source ? source(path, error) : nullptr;
        const AssetDependencies dependencies{[](AssetId) -> const AssetBlobView * { return nullptr; }};
        if (blob == nullptr || !AircraftConfigDecoder().decode(*blob, dependencies, config, error))
        {
            if (error.empty())
                error = "could not load the blob";
            return false;
        }
        return true;
    }

    const AircraftConfig *AircraftConfigStore::Get(std::string_view path)
    {
        const std::string key(path);
        if (mFailed.contains(key))
            return nullptr;
        if (const auto found = mDecoded.find(key); found != mDecoded.end())
            return found->second.get();

        std::string error;
        auto config = std::make_unique<AircraftConfig>();
        if (!Decode(mSource, path, *config, error))
        {
            MIR_LOG_ERROR("Aircraft config '{}': {}", path, error);
            mFailed.emplace(key);
            return nullptr;
        }

        const AircraftConfig *decoded = config.get();
        mDecoded.emplace(key, std::move(config));
        return decoded;
    }

    AircraftConfig *AircraftConfigStore::Edit(std::string_view path)
    {
        const auto found = mDecoded.find(std::string(path));
        return found == mDecoded.end() ? nullptr : found->second.get();
    }

    void AircraftConfigStore::MarkEdited(std::string_view path)
    {
        mDirty.emplace(std::string(path));
        ++mEditGeneration;
    }

    bool AircraftConfigStore::IsDirty(std::string_view path) const
    {
        return mDirty.contains(std::string(path));
    }

    void AircraftConfigStore::ClearDirty(std::string_view path) { mDirty.erase(std::string(path)); }

    std::vector<std::string> AircraftConfigStore::TakeOverwrittenEdits()
    {
        std::vector<std::string> taken = std::move(mOverwritten);
        mOverwritten.clear();
        return taken;
    }

    void AircraftConfigStore::PrepareReload(Source candidate)
    {
        mStaged.clear();
        std::unordered_map<std::string, AircraftConfig> staged;
        for (const auto &entry : mDecoded)
        {
            const std::string &path = entry.first;
            AircraftConfig replacement;
            std::string error;
            if (!Decode(candidate, path, replacement, error))
                throw std::runtime_error("aircraft config reload rejected: '" + path + "': " + error);
            staged.emplace(path, std::move(replacement));
        }
        for (const std::string &path : mFailed)
        {
            AircraftConfig replacement;
            std::string error;
            if (Decode(candidate, path, replacement, error))
                staged.emplace(path, std::move(replacement));
        }
        mStaged = std::move(staged);
    }

    void AircraftConfigStore::CommitReload()
    {
        for (auto &[path, staged] : mStaged)
        {
            if (mDirty.erase(path) != 0)
                mOverwritten.push_back(path);
            if (const auto found = mDecoded.find(path); found != mDecoded.end())
            {
                *found->second = std::move(staged);
                continue;
            }
            mFailed.erase(path);
            mDecoded.emplace(path, std::make_unique<AircraftConfig>(std::move(staged)));
        }
        mStaged.clear();
        ++mGeneration;
    }

    void AircraftConfigStore::AbortReload() { mStaged.clear(); }

    AircraftConfigStore::Source MakeAircraftConfigSource(AssetCache &assets, const AssetResolver &resolver)
    {
        return [&assets, &resolver](std::string_view path, std::string &error) -> const AssetBlobView *
        {
            const AssetId id = resolver.Resolve(path, &error);
            if (id.IsNull())
                return nullptr;

            const AssetBlobView *blob = assets.Load(id);
            if (blob == nullptr)
                error = "could not load '" + resolver.PathOf(id) + "'";
            return blob;
        };
    }
}
