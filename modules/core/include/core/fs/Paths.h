/**
 * @file Paths.h
 * @author Sumin Park
 * @brief Executable-relative asset paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <string_view>

namespace mir
{
    // Full path of the running executable; empty if unknown
    const std::filesystem::path &ExecutablePath();

    const std::filesystem::path &ExecutableDir();

    // A pinned immutable generation directory; empty falls back to <exe>/shaders.
    // The renderer sets it for a whole prepare/commit so every ShaderPath read
    // resolves inside one generation.
    void SetShaderRootOverride(std::filesystem::path path);
    void ClearShaderRootOverride();
    std::filesystem::path ShaderRootOverride();

    std::filesystem::path ShaderPath(std::string_view name);
    std::filesystem::path FontPath(std::string_view name);
    std::filesystem::path BrandingPath(std::string_view name);

    std::filesystem::path CookedAssetsDir();

    // engine.log and last-error.json; beside the exe so it never depends on the launch directory
    std::filesystem::path LogDir();

    // Per-user files: %LOCALAPPDATA%/Mjolnir on Windows, next to the executable elsewhere
    std::filesystem::path UserDataDir();
}
