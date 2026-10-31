/**
 * @file AssetSettings.h
 * @author sumin.park
 * @brief Project_settings.json's "assets" section.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>

namespace mir
{
    struct AssetSettings
    {
        static constexpr uint32_t kDefaultLoadThreads = 2;
        static constexpr uint32_t kMaxLoadThreads = 32;

        // How many background loads run at once. The job queue lowers it to the worker count.
        uint32_t loadThreads = kDefaultLoadThreads;

        static bool ValidLoadThreads(double value);

        // The settings in the file at path.
        static AssetSettings LoadFile(const std::filesystem::path &path);
    };
}
