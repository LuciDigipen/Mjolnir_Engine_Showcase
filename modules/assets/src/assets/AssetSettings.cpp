/**
 * @file AssetSettings.cpp
 * @author sumin.park
 * @brief Project_settings.json's "assets" section.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "assets/AssetSettings.h"

#include <core/log/Log.h>
#include <project/ProjectSettings.h>

#include <nlohmann/json.hpp>

#include <cmath>

namespace mir
{
    bool AssetSettings::ValidLoadThreads(double value)
    {
        return std::isfinite(value) && std::trunc(value) == value && value >= 1.0 && value <= kMaxLoadThreads;
    }

    AssetSettings AssetSettings::LoadFile(const std::filesystem::path &path)
    {
        AssetSettings settings;
        const std::optional<nlohmann::json> root = ReadProjectSettings(path);
        if (!root)
            return settings;

        // find() so an "assets" that is null or a scalar means defaults, not a type error
        const auto section = root->find("assets");
        if (section == root->end() || !section->is_object())
            return settings;

        const auto threads = section->find("loadThreads");
        if (threads == section->end())
            return settings;

        if (threads->is_number() && ValidLoadThreads(threads->get<double>()))
            settings.loadThreads = threads->get<uint32_t>();
        else
            MIR_LOG_WARN("assets.loadThreads in {} must be a whole number from 1 to {}, using {}", path.string(),
                kMaxLoadThreads, settings.loadThreads);
        return settings;
    }
}
