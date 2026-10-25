/**
 * @file ProjectSettings.h
 * @author sumin.park
 * @brief Project_settings.json validation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>

namespace mir
{
    // Check ProjectSetting.json if it's valid, nullopt when the file does not exist
    std::optional<nlohmann::json> ReadProjectSettings(const std::filesystem::path &path);
}// namespace mir
