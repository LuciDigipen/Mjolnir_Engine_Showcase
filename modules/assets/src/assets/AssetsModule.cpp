/**
 * @file AssetsModule.cpp
 * @author sumin.park
 * @brief Module in charge of assets control.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <assets/AssetsModule.h>

#include <core/log/Assert.h>
#include <core/log/Log.h>
#include <core/log/SetupError.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace mir
{
    AssetsModule::AssetsModule(std::filesystem::path cookedDir) : mCookedDir(std::move(cookedDir))
    {
        const std::filesystem::path manifestPath = mCookedDir / kAssetManifestFileName;

        std::error_code ec;
        mHadManifest = std::filesystem::exists(manifestPath, ec);
        if (mHadManifest)
        {
            std::optional<AssetManifest> manifest = AssetManifest::LoadFile(manifestPath);
            if (!manifest.has_value())
            {
                throw SetupError(ErrorCategory::BuildOutput, "The cooked asset manifest is corrupt", manifestPath,
                    "it could not be parsed as an asset manifest", "Rebuild to re-cook the assets.");
            }
            mManifest = std::make_shared<AssetManifest>(std::move(*manifest));
        }
        else
        {
            mManifest = std::make_shared<AssetManifest>();
            MIR_LOG_INFO("No asset manifest at '{}', starting with no cooked assets", manifestPath.string());
        }

        mCache.emplace(mManifest.get(), mCookedDir);
    }

    void AssetsModule::BeginReload()
    {
        AbortReload();

        const std::filesystem::path manifestPath = mCookedDir / kAssetManifestFileName;
        std::optional<AssetManifest> manifest = AssetManifest::LoadFile(manifestPath);
        if (!manifest.has_value())
            throw std::runtime_error("the new asset manifest could not be loaded");

        mCandidateManifest = std::make_shared<AssetManifest>(std::move(*manifest));
        mCandidate.emplace(mCandidateManifest.get(), mCookedDir);
    }

    AssetCache &AssetsModule::Candidate()
    {
        MIR_ASSERT(mCandidate.has_value(), "AssetsModule::Candidate: only between BeginReload and Commit/AbortReload");
        return *mCandidate;
    }

    void AssetsModule::CommitReload()
    {
        MIR_ASSERT(mCandidate.has_value(), "AssetsModule::CommitReload: no BeginReload");
        if (!mCandidate.has_value())
            return;

        // emplace reuses the optional's storage, so Cache() keeps its address
        mCache.reset();
        mCandidate.reset();
        mManifest = std::move(mCandidateManifest);
        mCache.emplace(mManifest.get(), mCookedDir);
        ++mReloadEpoch;
    }

    void AssetsModule::AbortReload()
    {
        mCandidate.reset();
        mCandidateManifest.reset();
    }
}
