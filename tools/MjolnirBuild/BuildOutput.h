/**
 * @file BuildOutput.h
 * @author Quy Dong Ta
 * @brief The folder MjolnirBuild writes: which folders it refuses, and the staged swap that replaces a previous build.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mir::build
{
    // In every build, so the next build knows the folder is its own to replace
    inline constexpr std::string_view kBuildMarkerFileName = "mjolnir_build.json";

    // The longest path Windows and the engine open without long-path support
    inline constexpr std::size_t kMaxPathLength = 259;

    //in the on-disk spelling of what exists so two spellings of one folder compare equal
    std::filesystem::path NormalizedPath(const std::filesystem::path &path);

    // path is dir or below it
    bool IsWithin(const std::filesystem::path &path, const std::filesystem::path &dir);

    // UTF-8, for messages and command lines
    std::string PathText(const std::filesystem::path &path);

    struct OutputRules
    {
        // The checkout holding assets/ and games/
        std::filesystem::path mSourceRoot;
        std::filesystem::path mGameBuildDir;
    };

    // Why out, or the .tmp and .old folders a build deletes beside it, cannot be used, nullopt when they can
    std::optional<std::string> CheckOutputDir(const std::filesystem::path &out, const OutputRules &rules);

    // Why a file MjolnirBuild writes beside the build, named by option (--result, --progress), would be lost with out,
    // its .tmp or its .old; nullopt when it would not
    std::optional<std::string> ReportFileProblem(const std::filesystem::path &file, const std::filesystem::path &out,
        std::string_view option);

    // Writes a --result or --progress file, making its folder, and retries for a moment while a reader has it open
    bool WriteReportFile(const std::filesystem::path &file, std::string_view text);

    // Why a file longestRelative characters below out could not be opened, or nullopt
    std::optional<std::string> CheckPathLength(const std::filesystem::path &out, std::size_t longestRelative);

    bool IsBuildDir(const std::filesystem::path &dir);

    // <out>.tmp, beside out
    std::filesystem::path StagingDir(const std::filesystem::path &out);

    // <out>.old, where the previous build waits while the new one moves in
    std::filesystem::path PreviousDir(const std::filesystem::path &out);

    // An empty, marked staging folder for out, replacing one a failed build left. Throws std::runtime_error
    std::filesystem::path BeginStaging(const std::filesystem::path &out);

    // Moves staging into out's place. Throws std::runtime_error, leaving out as it was
    void CommitStaging(const std::filesystem::path &staging, const std::filesystem::path &out);

    void AbandonStaging(const std::filesystem::path &staging) noexcept;
}
