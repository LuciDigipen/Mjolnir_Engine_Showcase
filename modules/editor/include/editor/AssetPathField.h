/**
 * @file AssetPathField.h
 * @author Jackey Cen
 * @brief The candidates an asset-path field offers from the source assets root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // Sorted forward-slash paths of the regular files under root whose name ends in suffix, case-insensitive.
    // Empty when root is missing or unreadable.
    std::vector<std::string> AssetPathCandidates(const std::filesystem::path &root, std::string_view suffix);

    // True when the stored path names a regular file under root; empty never does.
    bool AssetPathExists(const std::filesystem::path &root, std::string_view assetsRelativePath);

    // True when path and its terminator fit a String field.
    bool AssetPathFitsField(std::string_view path);
} // namespace mir
