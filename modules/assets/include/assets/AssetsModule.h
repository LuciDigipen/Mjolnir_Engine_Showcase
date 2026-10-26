/**
 * @file AssetsModule.h
 * @author sumin.park
 * @brief Owns the cooked asset manifest and cache, and swaps in a new cook.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>

#include <filesystem>
#include <memory>
#include <optional>

namespace mir
{
    class AssetsModule
    {
    public:
        // Loads the manifest now. Missing = no cooked assets (a build with no assets
        // writes none); corrupt throws SetupError (BuildOutput)
        explicit AssetsModule(std::filesystem::path cookedDir);

        AssetsModule(const AssetsModule &) = delete;
        AssetsModule &operator=(const AssetsModule &) = delete;

        // Never null, and the same object across a reload, so holders keep their pointer
        AssetCache &Cache() { return *mCache; }
        const AssetManifest &Manifest() const { return *mManifest; }

        // false when startup found no manifest
        bool HadManifest() const { return mHadManifest; }

        // A reload: BeginReload, then holders prepare from Candidate(), then CommitReload
        // swaps it in, or AbortReload drops it. BeginReload throws for a manifest it cannot parse
        void BeginReload();
        AssetCache &Candidate();
        void CommitReload();
        void AbortReload();

    private:
        std::filesystem::path mCookedDir;
        bool mHadManifest = false;

        // the caches hold a raw pointer to their manifest: manifest first, so it dies last,
        // and on the heap, so the address survives the swap
        std::unique_ptr<AssetManifest> mManifest;
        std::optional<AssetCache> mCache;

        std::unique_ptr<AssetManifest> mCandidateManifest;
        std::optional<AssetCache> mCandidate;
    };
}
