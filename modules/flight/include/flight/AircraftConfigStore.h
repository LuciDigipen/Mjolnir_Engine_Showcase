/**
 * @file AircraftConfigStore.h
 * @author Jackey Cen
 * @brief Decodes each aircraft configuration path once and keeps the result.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
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

        // Decodes every cached path through the candidate source; a cached success that no longer
        // decodes vetoes the whole reload with std::runtime_error and stages nothing.
        void PrepareReload(Source candidate);
        // Swaps the staged configs into their existing objects, so held pointers stay valid.
        void CommitReload();
        void AbortReload();

        // Changes only on a commit.
        uint32_t Generation() const { return mGeneration; }

      private:
        bool Decode(const Source &source, std::string_view path, AircraftConfig &config, std::string &error) const;

        Source mSource;
        std::unordered_map<std::string, std::unique_ptr<AircraftConfig>> mDecoded;
        std::unordered_set<std::string> mFailed;
        std::unordered_map<std::string, AircraftConfig> mStaged;
        uint32_t mGeneration = 0;
    };

    // Synchronous, through the cooked assets, like the script loader.
    AircraftConfigStore::Source MakeAircraftConfigSource(AssetCache &assets, const AssetResolver &resolver);
}
