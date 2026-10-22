/**
 * @file PostProcessingState.h
 * @author Rahul Nair
 * @brief The Post-Processing panel's per-effect entries resolved into the
 *        exact values VulkanRenderer applies for a frame - defaults for a
 *        missing entry, neutral values for a disabled one, AA clamped.
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

namespace mir
{
    struct PostProcessingState
    {
        // density is 0 while Fog is disabled.
        FogParams fog;

        // exposure 1 and None while HDR is disabled.
        HdrParams hdr;

        // intensity is 0 while Bloom is disabled - the tonemap pass still adds Bloom's output.
        bool bloomEnabled = false;
        BloomParams bloom;

        bool depthOfFieldEnabled = false;
        DepthOfFieldParams depthOfField;

        // sampleCount is at least 1.
        bool motionBlurEnabled = false;
        MotionBlurParams motionBlur;

        // mode is Off while Anti-Aliasing is disabled, then clamped by ClampAntiAliasingMode;
        // rtSampleCount is clamped by ClampRtSampleCount.
        AntiAliasingParams antiAliasing;

        // grain/contrast/saturation/vignetteStrength are neutral while Film is disabled.
        FilmParams film;
    };

    PostProcessingState ResolvePostProcessing(const PostProcessingSettings &settings, bool rayTracingActive,
        bool dlssAvailable);
}
