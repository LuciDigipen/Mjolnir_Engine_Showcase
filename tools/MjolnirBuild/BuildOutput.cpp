/**
 * @file BuildOutput.cpp
 * @author Quy Dong Ta
 * @brief Output folder checks and the staged swap.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "BuildOutput.h"

#include <core/fs/FileIO.h>

#include <stdexcept>
#include <system_error>
#include <utility>

namespace mir::build
{
    namespace fs = std::filesystem;

    fs::path NormalizedPath(const fs::path &path)
    {
        std::error_code ec;
        fs::path result = fs::weakly_canonical(fs::absolute(path, ec), ec);
        if (ec)
            result = fs::absolute(path).lexically_normal();
        if (!result.has_filename() && result.has_relative_path())
            result = result.parent_path();
        return result;
    }

    bool IsWithin(const fs::path &path, const fs::path &dir)
    {
        const fs::path relative = path.lexically_relative(dir);
        return !relative.empty() && *relative.begin() != "..";
    }

    std::string PathText(const fs::path &path)
    {
        const std::u8string text = path.u8string();
        return { reinterpret_cast<const char *>(text.data()), text.size() };
    }

    namespace
    {
        std::optional<std::string> CheckLocation(const fs::path &dir, const std::string &name, const fs::path &source,
            const fs::path &player)
        {
            if (dir == dir.root_path())
                return name + " is a drive root";
            if (IsWithin(source, dir))
                return name + " holds the source tree";
            if (IsWithin(dir, source) && !IsWithin(dir.parent_path(), source / "builds"))
                return name + " is inside the source tree; use a folder under builds/ or outside the checkout";
            if (IsWithin(dir, player) || IsWithin(player, dir))
                return name + " overlaps the player template " + PathText(player);

            std::error_code ec;
            for (fs::path parent = dir;; parent = parent.parent_path())
            {
                if (fs::exists(parent / "CMakeCache.txt", ec))
                    return name + " is inside the build folder " + PathText(parent) + "; use a folder of its own";
                if (parent == parent.parent_path())
                    break;
            }
            return std::nullopt;
        }
    }

    std::optional<std::string> CheckOutputDir(const fs::path &outDir, const OutputRules &rules)
    {
        if (outDir.empty())
            return "no output folder was given";

        const fs::path out = NormalizedPath(outDir);
        const fs::path source = NormalizedPath(rules.mSourceRoot);
        const fs::path player = NormalizedPath(rules.mPlayerDir);
        const fs::path staging = StagingDir(out);
        const fs::path previous = PreviousDir(out);

        
        for (const auto &[dir, name] : { std::pair{ out, PathText(out) },
                 std::pair{ staging, PathText(staging) + ", where the build is staged," },
                 std::pair{ previous, PathText(previous) + ", where the previous build is moved aside," } })
        {
            if (std::optional<std::string> problem = CheckLocation(dir, name, source, player))
                return problem;
        }

        std::error_code ec;
        if (fs::exists(out, ec) && !fs::is_directory(out, ec))
            return PathText(out) + " is a file";
        if (fs::is_directory(out, ec) && !fs::is_empty(out, ec) && !IsBuildDir(out))
            return PathText(out) + " is not empty and holds no previous build; empty it or pick another folder";
        for (const fs::path &dir : { staging, previous })
        {
            if (fs::exists(dir, ec) && !IsBuildDir(dir))
                return PathText(dir) + " already exists and is not a build; move or delete it";
        }

        return std::nullopt;
    }

    std::optional<std::string> CheckPathLength(const fs::path &out, std::size_t longestRelative)
    {
        const std::size_t longest = StagingDir(out).native().size() + 1 + longestRelative;
        if (longest <= kMaxPathLength)
            return std::nullopt;

        return "the build's longest path would be " + std::to_string(longest) + " characters, over the " +
            std::to_string(kMaxPathLength) + " Windows and the engine can open; use a shorter output folder";
    }

    bool IsBuildDir(const fs::path &dir)
    {
        std::error_code ec;
        return fs::is_regular_file(dir / kBuildMarkerFileName, ec);
    }

    fs::path StagingDir(const fs::path &out)
    {
        fs::path staging = NormalizedPath(out);
        staging += ".tmp";
        return staging;
    }

    fs::path PreviousDir(const fs::path &out)
    {
        fs::path previous = NormalizedPath(out);
        previous += ".old";
        return previous;
    }

    fs::path BeginStaging(const fs::path &out)
    {
        const fs::path staging = StagingDir(out);

        std::error_code ec;
        if (fs::exists(staging, ec))
        {
            if (!IsBuildDir(staging))
                throw std::runtime_error(PathText(staging) + " already exists and is not a build; move or delete it");
            fs::remove_all(staging, ec);
            if (ec)
                throw std::runtime_error("could not remove the failed build " + PathText(staging) + ": " + ec.message());
        }

        fs::create_directories(staging, ec);
        if (ec)
            throw std::runtime_error("could not create " + PathText(staging) + ": " + ec.message());
        if (!WriteText(staging / kBuildMarkerFileName, "{\n  \"version\": 1\n}\n"))
            throw std::runtime_error("could not write " + PathText(staging / kBuildMarkerFileName));

        return staging;
    }

    void CommitStaging(const fs::path &staging, const fs::path &outDir)
    {
        const fs::path out = NormalizedPath(outDir);
        const fs::path previous = PreviousDir(out);

        std::error_code ec;
        if (fs::exists(previous, ec))
        {
            if (!IsBuildDir(previous))
                throw std::runtime_error(PathText(previous) + " already exists and is not a build; move or delete it");
            fs::remove_all(previous, ec);
            if (ec)
                throw std::runtime_error("could not remove the old build " + PathText(previous) + ": " + ec.message());
        }

        bool movedAside = false;
        if (fs::is_directory(out, ec))
        {
            if (fs::is_empty(out, ec))
                fs::remove(out, ec);
            else
            {
                fs::rename(out, previous, ec);
                movedAside = !ec;
            }
            if (ec)
                throw std::runtime_error("could not move the previous build in " + PathText(out) + " aside (" +
                    ec.message() + "); is the game still running from it?");
        }

        fs::rename(staging, out, ec);
        if (ec)
        {
            std::error_code ignored;
            if (movedAside)
                fs::rename(previous, out, ignored);
            throw std::runtime_error("could not move the new build into " + PathText(out) + ": " + ec.message());
        }

        // a failure leaves it for the next build to remove
        if (movedAside)
            fs::remove_all(previous, ec);
    }

    void AbandonStaging(const fs::path &staging) noexcept
    {
        std::error_code ec;
        fs::remove_all(staging, ec);
    }
}
