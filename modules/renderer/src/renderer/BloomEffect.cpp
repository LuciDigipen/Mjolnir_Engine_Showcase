/**
 * @file BloomEffect.cpp
 * @author Rahul Nair
 * @brief Bloom's shape - the single source of truth for its default params
 *        and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/BloomEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

namespace mir
{
    namespace
    {
        void DrawBloomParams(nlohmann::json &params, const PostEffectContext &)
        {
            BloomParams bloom = ReadBloomParams(params);
            bool changed = false;

            changed |= ImGui::DragFloat("Threshold", &bloom.threshold, 0.01f, 0.0f, 16.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Intensity", &bloom.intensity, 0.01f, 0.0f, 8.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Radius", &bloom.radius, 0.01f, 0.0f, 4.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);

            if (changed)
                WriteBloomParams(params, bloom);
        }
    }

    BloomParams ReadBloomParams(const nlohmann::json &params)
    {
        BloomParams bloom;
        bloom.threshold = params.value("threshold", bloom.threshold);
        bloom.intensity = params.value("intensity", bloom.intensity);
        bloom.radius = params.value("radius", bloom.radius);
        return bloom;
    }

    void WriteBloomParams(nlohmann::json &params, const BloomParams &bloom)
    {
        params["threshold"] = bloom.threshold;
        params["intensity"] = bloom.intensity;
        params["radius"] = bloom.radius;
    }

    void RegisterBloomPostEffect()
    {
        PostEffectRegistry::Instance().Register(kBloomEffectName, &DrawBloomParams);
    }
}
