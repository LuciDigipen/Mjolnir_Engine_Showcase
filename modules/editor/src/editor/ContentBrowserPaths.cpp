/**
 * @file ContentBrowserPaths.cpp
 * @author Jackey Cen
 * @brief Content browser paths as seen by the source assets root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/ContentBrowserPaths.h>

namespace mir
{
    namespace
    {
        std::string_view Trimmed(std::string_view path)
        {
            while (!path.empty() && path.back() == '/')
                path.remove_suffix(1);
            return path;
        }
    } // namespace

    std::optional<std::string> BrowserAssetsRoot(std::string_view gameDir, std::string_view assetIdRoot)
    {
        const std::string_view directory = Trimmed(gameDir);
        const std::string_view root = Trimmed(assetIdRoot);
        if (root == directory)
            return std::string{};
        if (root.size() > directory.size() && root.compare(0, directory.size(), directory) == 0 &&
            root[directory.size()] == '/')
        {
            return std::string(root.substr(directory.size() + 1));
        }
        return std::nullopt;
    }

    std::string BrowserPathForAsset(std::string_view browserAssetsRoot, std::string_view assetsRelativePath)
    {
        const std::string_view root = Trimmed(browserAssetsRoot);
        if (root.empty())
            return std::string(assetsRelativePath);
        if (assetsRelativePath.empty())
            return std::string(root);
        return std::string(root) + '/' + std::string(assetsRelativePath);
    }

    std::optional<std::string> AssetsRelativeFolder(std::string_view assetsRoot, std::string_view folder)
    {
        const std::string_view root = Trimmed(assetsRoot);
        if (root.empty())
            return std::nullopt;
        const std::string_view selected = Trimmed(folder);
        if (selected == root)
            return std::string{};
        if (selected.size() > root.size() && selected.compare(0, root.size(), root) == 0 &&
            selected[root.size()] == '/')
        {
            return std::string(selected.substr(root.size() + 1));
        }
        return std::nullopt;
    }
} // namespace mir
