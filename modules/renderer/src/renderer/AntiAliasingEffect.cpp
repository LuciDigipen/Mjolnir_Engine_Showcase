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
                return context.rayTracing && context.dlssAvailable;
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
                // Only the sizes the shader has a standard sample pattern for.
                constexpr std::array<int, 4> kSampleCounts{ 1, 2, 4, 8 };
                constexpr std::array<const char *, kSampleCounts.size()> kSampleCountNames{ "1x", "2x", "4x", "8x" };
                const int current = ClampRtSampleCount(aa.rtSampleCount);
                int sampleIndex = 0;
                for (int i = 0; i < static_cast<int>(kSampleCounts.size()); ++i)
                    if (kSampleCounts[static_cast<size_t>(i)] == current)
                        sampleIndex = i;

                if (ImGui::Combo("Ray Samples", &sampleIndex, kSampleCountNames.data(),
                    static_cast<int>(kSampleCountNames.size())))
                {
                    aa.rtSampleCount = kSampleCounts[static_cast<size_t>(sampleIndex)];
                    changed = true;
                }
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

    AntiAliasingMode ClampAntiAliasingMode(AntiAliasingMode requested, bool rayTracing, bool dlssAvailable)
    {
        return ModeAllowed(requested, PostEffectContext{ rayTracing, dlssAvailable }) ? requested
            : AntiAliasingMode::Off;
    }

    int ClampRtSampleCount(int requested)
    {
        if (requested <= 1)
            return 1;
        if (requested <= 2)
            return 2;
        if (requested <= 4)
            return 4;
        return 8;
    }

    void RegisterAntiAliasingPostEffect()
    {
        PostEffectRegistry::Instance().Register(kAntiAliasingEffectName, &DrawAntiAliasingParams);
    }
}
