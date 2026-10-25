/**
 * @file SetupError.h
 * @author sumin.park
 * @brief A fatal startup failure, thrown out of setup and reported by main.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <exception>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace mir
{
    // Grouped by what the reader does to fix it, so the exit code alone says what to do next
    enum class ErrorCategory
    {
        Usage,       // fix the command line
        ProjectData, // fix or restore the game's files
        BuildOutput, // rebuild
        System,      // fix the machine: drivers, devices, permissions
        Internal,    // report a bug
    };

    // Public contract: scripts and agents branch on these, so never renumber one
    inline constexpr int ExitCodeOf(ErrorCategory category)
    {
        switch (category)
        {
        case ErrorCategory::Usage:
            return 10;
        case ErrorCategory::ProjectData:
            return 11;
        case ErrorCategory::BuildOutput:
            return 12;
        case ErrorCategory::System:
            return 13;
        case ErrorCategory::Internal:
            return 14;
        }
        return 14;
    }

    // The run finished, but something failed while shutting down; the log says what
    inline constexpr int kShutdownExitCode = 15;

    inline constexpr std::string_view CategoryName(ErrorCategory category)
    {
        switch (category)
        {
        case ErrorCategory::Usage:
            return "usage";
        case ErrorCategory::ProjectData:
            return "project-data";
        case ErrorCategory::BuildOutput:
            return "build-output";
        case ErrorCategory::System:
            return "system";
        case ErrorCategory::Internal:
            return "internal";
        }
        return "internal";
    }

    class SetupError final : public std::exception
    {
    public:
        SetupError(ErrorCategory category, std::string what, std::filesystem::path path, std::string cause,
            std::string fix)
            : mCategory(category), mWhat(std::move(what)), mPath(std::move(path)), mCause(std::move(cause)),
            mFix(std::move(fix))
        {}

        const char *what() const noexcept override { return mWhat.c_str(); }

        ErrorCategory Category() const { return mCategory; }
        const std::string &What() const { return mWhat; }
        const std::filesystem::path &Path() const { return mPath; }
        const std::string &Cause() const { return mCause; }
        const std::string &Fix() const { return mFix; }

    private:
        ErrorCategory mCategory;
        std::string mWhat;
        std::filesystem::path mPath;
        std::string mCause;
        std::string mFix;
    };
}
