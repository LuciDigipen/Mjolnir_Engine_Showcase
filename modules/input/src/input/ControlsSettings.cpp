/**
 * @file ControlsSettings.cpp
 * @author Quy Dong Ta
 * @brief The player's look options, inverted mouse Y and inverted gamepad Y,
 *        kept in a per-user file between runs.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/ControlsSettings.h>

#include <core/log/Log.h>
#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <system_error>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        constexpr const char *kSection = "controls";

        void ReadFlag(const json &section, const char *name, bool &out, std::string &problems)
        {
            const auto value = section.find(name);
            if (value == section.end())
                return;

            if (value->is_boolean())
                out = value->get<bool>();
            else
                problems += std::string(problems.empty() ? "" : "; ") + name + " is not true or false";
        }
    }

    ControlsSettings LoadControlsSettings(const std::filesystem::path &path)
    {
        ControlsSettings settings;

        std::error_code ec;
        if (!std::filesystem::exists(path, ec))
            return settings;

        std::string error;
        const std::optional<json> root = ReadJsonFile(path, &error);
        if (!root || !root->is_object())
        {
            MIR_LOG_WARN("controls settings at '{}' are not a readable JSON object; using the defaults", path.string());
            return settings;
        }

        const auto section = root->find(kSection);
        if (section == root->end())
            return settings;

        std::string problems;
        if (section->is_object())
        {
            ReadFlag(*section, "invertMouseY", settings.mInvertMouseY, problems);
            ReadFlag(*section, "invertGamepadY", settings.mInvertGamepadY, problems);
        }
        else
            problems = "controls is not an object";

        if (!problems.empty())
            MIR_LOG_WARN("controls settings at '{}': {}; the default is used instead", path.string(), problems);

        MIR_LOG_INFO("controls settings from '{}': invert mouse Y {}, invert gamepad Y {}", path.string(),
            settings.mInvertMouseY, settings.mInvertGamepadY);
        return settings;
    }

    bool SaveControlsSettings(const std::filesystem::path &path, const ControlsSettings &settings)
    {
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec)
        {
            MIR_LOG_ERROR("controls settings: cannot create '{}' ({})", path.parent_path().string(), ec.message());
            return false;
        }

        return UpdateJsonFile(path, [&](json &root)
            {
                root[kSection] = {{"invertMouseY", settings.mInvertMouseY},
                                  {"invertGamepadY", settings.mInvertGamepadY}};
            });
    }
}
