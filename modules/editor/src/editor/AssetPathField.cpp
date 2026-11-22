/**
 * @file AssetPathField.cpp
 * @author Jackey Cen
 * @brief The candidates an asset-path field offers from the source assets root.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/AssetPathField.h>

#include <core/ecs/ComponentFields.h>

#include <algorithm>
#include <cctype>
#include <system_error>

namespace mir
{
    namespace
    {
        char Lowered(char value)
        {
            return static_cast<char>(std::tolower(static_cast<unsigned char>(value)));
        }

        bool EndsWithIgnoringCase(std::string_view text, std::string_view suffix)
        {
            if (suffix.size() > text.size())
                return false;
            return std::equal(suffix.rbegin(), suffix.rend(), text.rbegin(),
                [](char left, char right) { return Lowered(left) == Lowered(right); });
        }
    } // namespace

    std::vector<std::string> AssetPathCandidates(const std::filesystem::path &root, std::string_view suffix)
    {
        std::vector<std::string> paths;
        if (root.empty())
            return paths;

        std::error_code error;
        std::filesystem::recursive_directory_iterator entry(root,
            std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        for (; !error && entry != end; entry.increment(error))
        {
            std::error_code fileError;
            if (!entry->is_regular_file(fileError))
                continue;
            const std::string name = entry->path().filename().string();
            if (!EndsWithIgnoringCase(name, suffix))
                continue;
            paths.push_back(entry->path().lexically_relative(root).generic_string());
        }

        std::sort(paths.begin(), paths.end());
        return paths;
    }

    bool AssetPathExists(const std::filesystem::path &root, std::string_view assetsRelativePath)
    {
        if (root.empty() || assetsRelativePath.empty())
            return false;
        std::error_code error;
        return std::filesystem::is_regular_file(root / std::filesystem::path(assetsRelativePath), error);
    }

    bool AssetPathFitsField(std::string_view path)
    {
        return path.size() < kFieldStringCapacity;
    }
} // namespace mir
