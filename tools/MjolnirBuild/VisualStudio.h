/**
 * @file VisualStudio.h
 * @author Quy Dong Ta
 * @brief Finds the Visual Studio whose vcvars64.bat the game build step loads before it compiles.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace mir::build
{
    struct VisualStudio
    {
        // Empty when this process already has a Developer environment, so nothing is loaded
        std::filesystem::path mVcvars;
        // e.g. 14.51.36231, the toolset a build folder was configured with; empty for vcvars' newest
        std::string mToolset;
    };

    // From the environment children inherit; nullopt when unset or empty
    std::optional<std::filesystem::path> EnvironmentPath(const wchar_t *name);

    // The Visual Studio and toolset of a CMakeCache.txt's CMAKE_CXX_COMPILER, while that cl.exe and vcvars64.bat exist
    std::optional<VisualStudio> CacheVisualStudio(const std::filesystem::path &cacheFile);

    // The newest install with the C++ tools, whether a release, a preview or the Build Tools
    std::optional<VisualStudio> NewestVisualStudio();

    // A Developer environment, then buildDir's cache, then the cache of the build editorDir is in, then the newest
    // install; nullopt when there is none
    std::optional<VisualStudio> FindVisualStudio(const std::filesystem::path &buildDir,
        const std::filesystem::path &editorDir);
}
