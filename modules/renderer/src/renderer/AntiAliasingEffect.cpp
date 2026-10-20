/**
 * @file AntiAliasingEffect.cpp
 * @author Rahul Nair
 * @brief Anti-aliasing's shape - the single source of truth for its default
 *        params and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/AntiAliasingEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

#include <array>

namespace mir
{
    namespace
    {
        constexpr std::array<AntiAliasingMode, 4> kAllModes{
            AntiAliasingMode::Off, AntiAliasingMode::Fxaa, AntiAliasingMode::Msaa, AntiAliasingMode::Dlss };

        bool ModeAllowed(AntiAliasingMode mode, const PostEffectContext &context)
        {
            switch (mode)
            {
            case AntiAliasingMode::Off:
                return true;
            case AntiAliasingMode::Fxaa:
                return !context.rayTracing;
            case AntiAliasingMode::Msaa:
                return context.rayTracing;
            case AntiAliasingMode::Dlss:
                return context.rayTracing && context.ngxSupported;
            }
            return false;
        }

        void DrawAntiAliasingParams(nlohmann::json &params, const PostEffectContext &context)
        {
            AntiAliasingParams aa = ReadAntiAliasingParams(params);
            bool changed = false;

            std::array<AntiAliasingMode, kAllModes.size()> allowedModes{};
            std::array<const char *, kAllModes.size()> allowedNames{};
            int allowedCount = 0;
            int currentIndex = 0;
            for (const AntiAliasingMode mode : kAllModes)
            {
                if (!ModeAllowed(mode, context))
                    continue;
                if (mode == aa.mode)
                    currentIndex = allowedCount;
                allowedModes[allowedCount] = mode;
                allowedNames[allowedCount] = ToString(mode).data();
                ++allowedCount;
            }

            bool currentIsAllowed = false;
            for (int i = 0; i < allowedCount; ++i)
                currentIsAllowed |= allowedModes[static_cast<size_t>(i)] == aa.mode;
            if (!currentIsAllowed)
                currentIndex = 0;

            if (ImGui::Combo("Mode", &currentIndex, allowedNames.data(), allowedCount))
            {
                aa.mode = allowedModes[static_cast<size_t>(currentIndex)];
                changed = true;
            }

            if (aa.mode == AntiAliasingMode::Msaa)
            {
                changed |= ImGui::DragInt("Ray Samples", &aa.rtSampleCount, 0.1f, 1, 4, "%d",
                    ImGuiSliderFlags_AlwaysClamp);
            }

            if (changed)
                WriteAntiAliasingParams(params, aa);
        }
    }

    std::string_view ToString(AntiAliasingMode mode)
    {
        switch (mode)
        {
        case AntiAliasingMode::Off:
            return "off";
        case AntiAliasingMode::Fxaa:
            return "fxaa";
        case AntiAliasingMode::Msaa:
            return "msaa";
        case AntiAliasingMode::Dlss:
            return "dlss";
        }
        return "off";
    }

    AntiAliasingMode ParseMode(std::string_view name)
    {
        if (name == "fxaa")
            return AntiAliasingMode::Fxaa;
        if (name == "msaa")
            return AntiAliasingMode::Msaa;
        if (name == "dlss")
            return AntiAliasingMode::Dlss;
        return AntiAliasingMode::Off;
    }

    AntiAliasingParams ReadAntiAliasingParams(const nlohmann::json &params)
    {
        AntiAliasingParams aa;
        aa.mode = ParseMode(params.value("mode", std::string(ToString(aa.mode))));
        aa.rtSampleCount = params.value("rtSampleCount", aa.rtSampleCount);
        return aa;
    }

    void WriteAntiAliasingParams(nlohmann::json &params, const AntiAliasingParams &aa)
    {
        params["mode"] = std::string(ToString(aa.mode));
        params["rtSampleCount"] = aa.rtSampleCount;
    }

    AntiAliasingMode ClampAntiAliasingMode(AntiAliasingMode requested, bool rayTracing, bool ngxSupported)
    {
        return ModeAllowed(requested, PostEffectContext{ rayTracing, ngxSupported }) ? requested
            : AntiAliasingMode::Off;
    }

    void RegisterAntiAliasingPostEffect()
    {
        PostEffectRegistry::Instance().Register(kAntiAliasingEffectName, &DrawAntiAliasingParams);
    }
}
