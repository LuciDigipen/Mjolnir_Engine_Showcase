/**
 * @file HdrEffect.cpp
 * @author Rahul Nair
 * @brief HDR tonemapping's shape - the single source of truth for its default
 *        params and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/HdrEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

#include <array>
#include <string>

namespace mir
{
    namespace
    {
        void DrawHdrParams(nlohmann::json &params, const PostEffectContext &)
        {
            HdrParams hdr = ReadHdrParams(params);
            bool changed = false;

            changed |= ImGui::DragFloat("Exposure", &hdr.exposure, 0.01f, 0.01f, 16.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);

            constexpr std::array<const char *, 3> kOperatorNames{ "None", "Reinhard", "ACES" };
            int operatorIndex = static_cast<int>(hdr.op);
            if (ImGui::Combo("Tonemap", &operatorIndex, kOperatorNames.data(),
                static_cast<int>(kOperatorNames.size())))
            {
                hdr.op = static_cast<TonemapOperator>(operatorIndex);
                changed = true;
            }

            if (changed)
                WriteHdrParams(params, hdr);
        }
    }

    std::string_view ToString(TonemapOperator op)
    {
        switch (op)
        {
        case TonemapOperator::None:
            return "none";
        case TonemapOperator::Reinhard:
            return "reinhard";
        case TonemapOperator::ACES:
            return "aces";
        }
        return "reinhard";
    }

    TonemapOperator ParseOperator(std::string_view name)
    {
        if (name == "none")
            return TonemapOperator::None;
        if (name == "aces")
            return TonemapOperator::ACES;
        return TonemapOperator::Reinhard;
    }

    HdrParams ReadHdrParams(const nlohmann::json &params)
    {
        HdrParams hdr;
        hdr.exposure = params.value("exposure", hdr.exposure);
        hdr.op = ParseOperator(params.value("tonemapOperator", std::string(ToString(hdr.op))));
        return hdr;
    }

    void WriteHdrParams(nlohmann::json &params, const HdrParams &hdr)
    {
        params["exposure"] = hdr.exposure;
        params["tonemapOperator"] = std::string(ToString(hdr.op));
    }

    void RegisterHdrPostEffect()
    {
        PostEffectRegistry::Instance().Register(kHdrEffectName, &DrawHdrParams);
    }
}
