/**
 * @file AssetsModule.h
 * @author sumin.park
 * @brief Module in charge of assets control.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetCache.h>
#include <assets/AssetManifest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace mir
{
    class AssetsModule
    {
    public:
        explicit AssetsModule(std::filesystem::path cookedDir);

        AssetsModule(const AssetsModule &) = delete;
        AssetsModule &operator=(const AssetsModule &) = delete;

        AssetCache &Cache() { return *mCache; }
        const AssetManifest &Manifest() const { return *mManifest; }
        // Keeps this manifest alive after a reload replaces it.
        std::shared_ptr<const AssetManifest> ManifestSnapshot() const { return mManifest; }

        // false when startup found no manifest
        bool HadManifest() const { return mHadManifest; }

        // Prepare replacements from Candidate() before applying the reload. An invalid manifest throws.
        void BeginReload();
        AssetCache &Candidate();
        // The manifest behind Candidate(), which also lists what this reload adds.
        const AssetManifest &CandidateManifest() const;
        void CommitReload();
        void AbortReload();

        // Changes after each successful reload.
        uint32_t ReloadEpoch() const { return mReloadEpoch; }

    private:
        std::filesystem::path mCookedDir;
        bool mHadManifest = false;
        uint32_t mReloadEpoch = 0;

        // the caches hold a raw pointer to their manifest: manifest first, so it dies last,
        // and on the heap, so the address survives the swap
        std::shared_ptr<AssetManifest> mManifest;
        std::optional<AssetCache> mCache;

        std::shared_ptr<AssetManifest> mCandidateManifest;
        std::optional<AssetCache> mCandidate;
    };
}
