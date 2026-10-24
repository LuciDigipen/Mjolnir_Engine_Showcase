/**
 * @file JsonFile.h
 * @author sumin.park
 * @brief Class for JSON control. Interpretates bytes from core/fs.FileIO.h
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace mir
{
    // after JSON already in memory. @p error, if given, receives why on failure
    std::optional<nlohmann::json> ParseJson(std::string_view text, std::string_view context,
        std::string *error = nullptr);

    // Nullopt for a missing file too, so @p error is the only way to tell it from a broken one
    std::optional<nlohmann::json> ReadJsonFile(const std::filesystem::path &path, std::string *error = nullptr);

    // Dump and write atomically
    bool WriteJsonFile(const std::filesystem::path &path, const nlohmann::json &root, int indent = 2);

    // Read @p path, hand the object to @p edit, write the result and report success/fail.
    bool UpdateJsonFile(const std::filesystem::path &path,
        const std::function<void(nlohmann::json &)> &edit,
        int indent = 2);
}
