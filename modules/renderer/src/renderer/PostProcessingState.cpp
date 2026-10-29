/**
 * @file PostProcessingState.cpp
 * @author Rahul Nair
 * @brief The Post-Processing panel's per-effect entries and the viewing
 *        Camera's lens resolved into the exact values VulkanRenderer applies
 *        for a frame - defaults for a missing entry, neutral values for a
 *        disabled one, AA clamped.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/PostProcessingState.h"

#include <algorithm>
#include <string>

namespace mir
{
    namespace
    {
        // Missing entries read as their defaults and count as disabled.
        template <typename Params>
        bool Resolve(const PostProcessingSettings &settings, std::string_view name,
            Params(*read)(const nlohmann::json &), Params &out)
        {
            const auto it = settings.effects.find(std::string(name));
            if (it == settings.effects.end())
            {
                out = Params{};
                return false;
            }
            out = read(it->second.params);
            return it->second.enabled;
        }
    }

    PostProcessingState ResolvePostProcessing(const PostProcessingSettings &settings, const Camera &lens,
        bool rayTracingActive, bool dlssAvailable)
    {
        PostProcessingState state;

        if (!Resolve(settings, kFogEffectName, &ReadFogParams, state.fog))
            state.fog.density = 0.0f;

        if (Resolve(settings, kHdrEffectName, &ReadHdrParams, state.hdr))
            state.exposure = std::max(lens.mExposure, 0.0f);
        else
            state.hdr.op = TonemapOperator::None;

        state.bloomEnabled = Resolve(settings, kBloomEffectName, &ReadBloomParams, state.bloom);
        if (!state.bloomEnabled)
            state.bloom.intensity = 0.0f;

        state.depthOfFieldEnabled =
            Resolve(settings, kDepthOfFieldEffectName, &ReadDepthOfFieldParams, state.depthOfField);
        state.focusDistance = std::max(lens.mFocusDistance, 0.0f);
        state.aperture = std::max(lens.mAperture, 0.0f);

        state.motionBlurEnabled = Resolve(settings, kMotionBlurEffectName, &ReadMotionBlurParams, state.motionBlur);
        state.motionBlur.sampleCount = std::max(state.motionBlur.sampleCount, 1);
        state.shutterStrength = std::max(lens.mShutterStrength, 0.0f);

        if (!Resolve(settings, kAntiAliasingEffectName, &ReadAntiAliasingParams, state.antiAliasing))
            state.antiAliasing.mode = AntiAliasingMode::Off;
        state.antiAliasing.mode = ClampAntiAliasingMode(state.antiAliasing.mode, rayTracingActive, dlssAvailable);
        state.antiAliasing.rtSampleCount = ClampRtSampleCount(state.antiAliasing.rtSampleCount);

        if (!Resolve(settings, kFilmEffectName, &ReadFilmParams, state.film))
        {
            state.film.grain = 0.0f;
            state.film.contrast = 1.0f;
            state.film.saturation = 1.0f;
            state.film.vignetteStrength = 0.0f;
        }

        return state;
    }
}
