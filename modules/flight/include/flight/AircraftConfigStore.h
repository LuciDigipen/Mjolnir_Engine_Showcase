/**
 * @file AircraftConfigStore.h
 * @author Jackey Cen
 * @brief Decodes each aircraft configuration path once, keeps the result and tracks unsaved edits.
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
#include <vector>

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

        // Mutable access to the loaded config, shared by every aircraft on that path; null if not loaded.
        AircraftConfig *Edit(std::string_view path);

        // Marks the path unsaved and bumps the edit generation, so the next fixed step re-applies the mass
        // properties without resetting engine, warning or camera state.
        void MarkEdited(std::string_view path);
        bool IsDirty(std::string_view path) const;
        void ClearDirty(std::string_view path);

        // Decodes every cached path through the candidate source; a cached success that no longer
        // decodes vetoes the whole reload with std::runtime_error and stages nothing.
        void PrepareReload(Source candidate);
        // Swaps the staged configs into their existing objects, so held pointers stay valid. A dirty
        // path takes the file's content and is reported by TakeOverwrittenEdits.
        void CommitReload();
        void AbortReload();

        // Paths whose unsaved edits a commit replaced; the store keeps no record after the take.
        std::vector<std::string> TakeOverwrittenEdits();

        // Changes on a reload commit.
        uint32_t Generation() const { return mGeneration; }
        // Changes on an edit.
        uint32_t EditGeneration() const { return mEditGeneration; }

      private:
        bool Decode(const Source &source, std::string_view path, AircraftConfig &config, std::string &error) const;

        Source mSource;
        std::unordered_map<std::string, std::unique_ptr<AircraftConfig>> mDecoded;
        std::unordered_set<std::string> mFailed;
        std::unordered_map<std::string, AircraftConfig> mStaged;
        std::unordered_set<std::string> mDirty;
        std::vector<std::string> mOverwritten;
        uint32_t mGeneration = 0;
        uint32_t mEditGeneration = 0;
    };

    // The store of the World's FlightModule, so a panel can edit through the World it draws.
    struct AircraftConfigStoreRef
    {
        AircraftConfigStore *store = nullptr;
    };


    // Synchronous, through the cooked assets, like the script loader.
    AircraftConfigStore::Source MakeAircraftConfigSource(AssetCache &assets, const AssetResolver &resolver);
}
