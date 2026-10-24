/**
 * @file FogEffect.cpp
 * @author Rahul Nair
 * @brief Distance fog's shape - the single source of truth for its default
 *        params and json layout, shared by RenderSystem (fallback/apply)
 *        and the Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/FogEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

#include <algorithm>

namespace mir
{
    namespace
    {
        void DrawFogParams(nlohmann::json &params, const PostEffectContext &)
        {
            FogParams fog = ReadFogParams(params);
            bool changed = false;

            changed |= ImGui::ColorEdit3("Color", &fog.color.x);
            changed |= ImGui::DragFloat("Density", &fog.density, 0.001f, 0.0f, 1.0f, "%.4f");
            changed |= ImGui::DragFloat("Start Distance", &fog.start, 1.0f, 0.0f, 10000.0f, "%.1f");
            changed |= ImGui::DragFloat("End Distance", &fog.end, 1.0f, 0.0f, 10000.0f, "%.1f");

            if (changed)
            {
                fog.end = std::max(fog.end, fog.start);
                WriteFogParams(params, fog);
            }
        }
    }

    FogParams ReadFogParams(const nlohmann::json &params)
    {
        FogParams fog;

        const auto colorIt = params.find("color");
        if (colorIt != params.end() && colorIt->is_array() && colorIt->size() >= 3)
        {
            fog.color = glm::vec3(colorIt->at(0).get<float>(), colorIt->at(1).get<float>(),
                colorIt->at(2).get<float>());
        }

        fog.density = params.value("density", fog.density);
        fog.start = params.value("start", fog.start);
        fog.end = params.value("end", fog.end);

        fog.end = std::max(fog.end, fog.start);
        return fog;
    }

    void WriteFogParams(nlohmann::json &params, const FogParams &fog)
    {
        params["color"] = { fog.color.x, fog.color.y, fog.color.z };
        params["density"] = fog.density;
        params["start"] = fog.start;
        params["end"] = fog.end;
    }

    void RegisterFogPostEffect()
    {
        PostEffectRegistry::Instance().Register(kFogEffectName, &DrawFogParams);
    }
}
