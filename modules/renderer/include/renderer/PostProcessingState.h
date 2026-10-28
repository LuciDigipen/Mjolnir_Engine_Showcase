/**
 * @file PostProcessingState.h
 * @author Rahul Nair
 * @brief The Post-Processing panel's per-effect entries and the viewing
 *        Camera's lens resolved into the exact values VulkanRenderer applies
 *        for a frame - defaults for a missing entry, neutral values for a
 *        disabled one, AA clamped.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/AntiAliasingEffect.h>
#include <renderer/BloomEffect.h>
#include <renderer/DepthOfFieldEffect.h>
#include <renderer/FilmEffect.h>
#include <renderer/FogEffect.h>
#include <renderer/HdrEffect.h>
#include <renderer/MotionBlurEffect.h>
#include <renderer/PostProcessingSettings.h>
#include <renderer/components/Camera.h>

namespace mir
{
    struct PostProcessingState
    {
        // density is 0 while Fog is disabled.
        FogParams fog;

        // None while HDR is disabled.
        HdrParams hdr;
        // The lens's, or 1 while HDR is disabled.
        float exposure = 1.0f;

        // intensity is 0 while Bloom is disabled - the tonemap pass still adds Bloom's output.
        bool bloomEnabled = false;
        BloomParams bloom;

        bool depthOfFieldEnabled = false;
        DepthOfFieldParams depthOfField;
        float focusDistance = 0.0f;
        float aperture = 0.0f;

        // sampleCount is at least 1.
        bool motionBlurEnabled = false;
        MotionBlurParams motionBlur;
        float shutterStrength = 0.0f;

        // mode is Off while Anti-Aliasing is disabled, then clamped by ClampAntiAliasingMode;
        // rtSampleCount is clamped by ClampRtSampleCount.
        AntiAliasingParams antiAliasing;

        // grain/contrast/saturation/vignetteStrength are neutral while Film is disabled.
        FilmParams film;
    };

    // @p lens is the Camera the frame is viewed through.
    PostProcessingState ResolvePostProcessing(const PostProcessingSettings &settings, const Camera &lens,
        bool rayTracingActive, bool dlssAvailable);
}
