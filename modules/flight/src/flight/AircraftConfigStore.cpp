/**
 * @file AircraftConfigStore.cpp
 * @author Jackey Cen
 * @brief Decodes each aircraft configuration path once and keeps the result.
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

#include <string>
#include <utility>

namespace mir
{
    AircraftConfigStore::AircraftConfigStore(Source source) : mSource(std::move(source)) {}

    const AircraftConfig *AircraftConfigStore::Get(std::string_view path)
    {
        const std::string key(path);
        if (mFailed.contains(key))
            return nullptr;
        if (const auto found = mDecoded.find(key); found != mDecoded.end())
            return found->second.get();

        std::string error;
        const AssetBlobView *blob = mSource ? mSource(path, error) : nullptr;
        auto config = std::make_unique<AircraftConfig>();
        const AssetDependencies dependencies{[](AssetId) -> const AssetBlobView * { return nullptr; }};
        if (blob == nullptr || !AircraftConfigDecoder().decode(*blob, dependencies, *config, error))
        {
            if (error.empty())
                error = "could not load the blob";
            MIR_LOG_ERROR("Aircraft config '{}': {}", path, error);
            mFailed.emplace(key);
            return nullptr;
        }

        const AircraftConfig *decoded = config.get();
        mDecoded.emplace(key, std::move(config));
        return decoded;
    }

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
