/**
 * @file PostProcessingSettings.cpp
 * @author Rahul Nair
 * @brief Project Settings' "postProcessing" section. A World resource,
 *        loaded and saved the same way RenderSettings is.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/PostProcessingSettings.h"

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <utility>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    std::optional<PostProcessingSettings> PostProcessingSettings::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
            return std::nullopt;

        PostProcessingSettings settings;

        const auto postIt = root->find("postProcessing");
        if (postIt == root->end() || !postIt->is_object())
            return settings;

        const auto effectsIt = postIt->find("effects");
        if (effectsIt == postIt->end() || !effectsIt->is_object())
            return settings;

        for (const auto &[name, entry] : effectsIt->items())
        {
            if (!entry.is_object())
                continue;

            PostEffectState state;
            state.enabled = entry.value("enabled", false);

            const auto paramsIt = entry.find("params");
            if (paramsIt != entry.end() && paramsIt->is_object())
                state.params = *paramsIt;

            settings.effects.emplace(name, std::move(state));
        }

        return settings;
    }

    bool PostProcessingSettings::SaveFile(const std::filesystem::path &path) const
    {
        return UpdateJsonFile(path, [this](json &root)
            {
                json effects = json::object();
                for (const auto &[name, state] : this->effects)
                    effects[name] = json{ { "enabled", state.enabled },{ "params", state.params } };
                root["postProcessing"] = json{ { "effects", std::move(effects) } };
            });
    }
}
