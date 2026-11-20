/**
 * @file TerrainBake.h
 * @author Jackey Cen
 * @brief The TerrainTool sync command the panel runs, its report, and the rule that drops a baked overlay.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <assets/AssetId.h>
#include <terrain/TerrainIdentity.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    struct TerrainBakeProject
    {
        std::filesystem::path mGameDir;
        std::filesystem::path mAssetsDir;
        std::string mIdRoot;
        std::filesystem::path mTool;
    };

    struct TerrainBakeRequest
    {
        std::filesystem::path mTool;
        std::filesystem::path mRecipe;
        std::string mIdRoot;
        std::string mSourcePath;
        std::filesystem::path mGameDir;
        std::filesystem::path mCandidateDir;
        std::filesystem::path mReportDirectory;
        AssetId mAsset;
    };

    // Null with error set when the recipe path does not resolve to a terrain asset.
    std::optional<TerrainBakeRequest> MakeTerrainBakeRequest(const TerrainBakeProject &project,
                                                             std::string_view recipePath, std::string &error);

    std::vector<std::string> TerrainBakeArguments(const TerrainBakeRequest &request,
                                                  const std::filesystem::path &reportFile);

    // The build writes this binding next to MjolnirEngine.
    std::filesystem::path ReadTerrainToolPath(const std::filesystem::path &workspaceFile);
    std::filesystem::path TerrainToolPath();

    enum class TerrainBakeOutcome : uint8_t
    {
        Published,
        UpToDate,
        Canceled,
        Failed,
    };

    struct TerrainBakeReport
    {
        TerrainBakeOutcome mOutcome = TerrainBakeOutcome::Failed;
        std::string mMessage;
        std::optional<TerrainReleaseId> mRelease;
        std::optional<TerrainSourceFingerprint> mFingerprint;
    };

    // A successful sync needs both a successful process exit and its completed report.
    TerrainBakeReport ReadTerrainBakeReport(const std::filesystem::path &reportFile, int exitCode);

    enum class TerrainBakeOverlayAction : uint8_t
    {
        Keep,
        Wait,
        Discard,
    };

    // Discards only once the asset has loaded the release the report names, and only when the layers did not
    // change since the bake started.
    TerrainBakeOverlayAction EvaluateTerrainBakeDiscard(const TerrainBakeReport &report, uint64_t startChange,
                                                        uint64_t currentChange, const TerrainReleaseInfo *loaded);
} // namespace mir
