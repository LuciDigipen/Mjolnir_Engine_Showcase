/**
 * @file PostProcessingSettings.h
 * @author Rahul Nair
 * @brief Project Settings' "postProcessing" section. A World resource,
 *        loaded and saved the same way RenderSettings is.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>

namespace mir
{
    struct PostEffectState
    {
        bool enabled = false;
        nlohmann::json params = nlohmann::json::object();

        bool operator==(const PostEffectState &) const = default;
    };

    struct PostProcessingSettings
    {
        std::unordered_map<std::string, PostEffectState> effects;

        bool operator==(const PostProcessingSettings &) const = default;

        static std::optional<PostProcessingSettings> LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };
}
