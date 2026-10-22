/**
 * @file MotionBlurEffect.cpp
 * @author Rahul Nair
 * @brief Motion blur's shape - the single source of truth for its default
 *        params and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/MotionBlurEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

namespace mir
{
    namespace
    {
        void DrawMotionBlurParams(nlohmann::json &params, const PostEffectContext &)
        {
            MotionBlurParams motionBlur = ReadMotionBlurParams(params);
            bool changed = false;

            changed |= ImGui::DragFloat("Shutter Strength", &motionBlur.shutterStrength, 0.01f, 0.0f, 2.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragInt("Sample Count", &motionBlur.sampleCount, 0.1f, 1, 32, "%d",
                ImGuiSliderFlags_AlwaysClamp);

            if (changed)
                WriteMotionBlurParams(params, motionBlur);
        }
    }

    MotionBlurParams ReadMotionBlurParams(const nlohmann::json &params)
    {
        MotionBlurParams motionBlur;
        motionBlur.shutterStrength = params.value("shutterStrength", motionBlur.shutterStrength);
        motionBlur.sampleCount = params.value("sampleCount", motionBlur.sampleCount);
        return motionBlur;
    }

    void WriteMotionBlurParams(nlohmann::json &params, const MotionBlurParams &motionBlur)
    {
        params["shutterStrength"] = motionBlur.shutterStrength;
        params["sampleCount"] = motionBlur.sampleCount;
    }

    void RegisterMotionBlurPostEffect()
    {
        PostEffectRegistry::Instance().Register(kMotionBlurEffectName, &DrawMotionBlurParams);
    }
}
