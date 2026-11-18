/**
 * @file AircraftConfigStore.h
 * @author Jackey Cen
 * @brief Decodes each aircraft configuration path once and keeps the result.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace mir
{
    struct AircraftConfig;
    class AssetCache;
    class AssetResolver;
    struct AssetBlobView;

    class AircraftConfigStore
    {
      public:
        // Returns the cooked blob for a path, or null with error set.
        using Source = std::function<const AssetBlobView *(std::string_view path, std::string &error)>;

        explicit AircraftConfigStore(Source source);

        // Null for a path that cannot be loaded or decoded; the failure is cached and logged once.
        const AircraftConfig *Get(std::string_view path);

      private:
        Source mSource;
        std::unordered_map<std::string, std::unique_ptr<AircraftConfig>> mDecoded;
        std::unordered_set<std::string> mFailed;
    };

    // Synchronous, through the cooked assets, like the script loader.
    AircraftConfigStore::Source MakeAircraftConfigSource(AssetCache &assets, const AssetResolver &resolver);
}
