/**
 * @file ContentBrowserPaths.h
 * @author Jackey Cen
 * @brief Content browser paths as seen by the source assets root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    // The assets root as Content Browser entries spell it, relative to the game directory.
    // Null when the assets root is outside the game directory.
    std::optional<std::string> BrowserAssetsRoot(std::string_view gameDir, std::string_view assetIdRoot);

    // The game-relative path the editor and its external tools use for an assets-relative path.
    std::string BrowserPathForAsset(std::string_view browserAssetsRoot, std::string_view assetsRelativePath);

    // The game-relative folder as a path under the source assets root: empty at the root itself,
    // null when the folder is outside it.
    std::optional<std::string> AssetsRelativeFolder(std::string_view assetsRoot, std::string_view folder);
} // namespace mir
