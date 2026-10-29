/**
 * @file DepthOfFieldEffect.cpp
 * @author Rahul Nair
 * @brief Depth of field's shape - the single source of truth for its default
 *        params and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/DepthOfFieldEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

namespace mir
{
    namespace
    {
        void DrawDepthOfFieldParams(nlohmann::json &params, const PostEffectContext &)
        {
            DepthOfFieldParams dof = ReadDepthOfFieldParams(params);
            bool changed = false;

            ImGui::TextDisabled("Focus distance and aperture are set on each Camera.");
            changed |= ImGui::DragFloat("Max Blur", &dof.maxBlur, 0.1f, 0.0f, 64.0f, "%.1f",
                ImGuiSliderFlags_AlwaysClamp);

            if (changed)
                WriteDepthOfFieldParams(params, dof);
        }
    }

    DepthOfFieldParams ReadDepthOfFieldParams(const nlohmann::json &params)
    {
        DepthOfFieldParams dof;
        dof.maxBlur = params.value("maxBlur", dof.maxBlur);
        return dof;
    }

    void WriteDepthOfFieldParams(nlohmann::json &params, const DepthOfFieldParams &dof)
    {
        params["maxBlur"] = dof.maxBlur;
    }

    void RegisterDepthOfFieldPostEffect()
    {
        PostEffectRegistry::Instance().Register(kDepthOfFieldEffectName, &DrawDepthOfFieldParams);
    }
}
