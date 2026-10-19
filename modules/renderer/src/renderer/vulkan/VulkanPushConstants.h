/**
 * @file VulkanPushConstants.h
 * @author Sumin Park
 * @brief CPU mirrors of the shaders' push constant blocks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>

#include <cstdint>

namespace mir::vk
{
    // normalCols = Normal matrix
    // vulkan's guaranteed minimum constant size = 128
    // vulkan pads every column to 16 anyways
    struct RasterPushConstants
    {
        glm::mat4 model;      // 64
        glm::vec4 normalCol0; // 16
        glm::vec4 normalCol1; // 16
        glm::vec4 normalCol2; // 16
        glm::vec4 tint;       // 16
    };

    // Ray gen writes at outputOffset so each view traces only its own rect.
    struct RtPushConstants
    {
        glm::uvec2 outputOffset;
    };

    // Shadow depth passes have no camera UBO, so the light's viewProj rides along.
    struct ShadowPushConstants
    {
        glm::mat4 viewProj; // 64
        glm::mat4 model;    // 64
    };

    // Must match TonemapOperator in renderer/HdrEffect.h and PushConstants in tonemap.slang.
    struct TonemapPushConstants
    {
        float exposure;
        uint32_t tonemapOperator;
        glm::vec2 invExtent;
        float bloomIntensity;
        float grain;
        float contrast;
        float saturation;
        float vignetteStrength;
        float vignetteRadius;
        float grainSeed;
    };

    struct BloomExtractPushConstants
    {
        float threshold;
        float knee;
        glm::vec2 invSrcExtent;
        glm::vec2 invTargetExtent;
    };

    struct BloomDownsamplePushConstants
    {
        glm::vec2 invSrcExtent;
        glm::vec2 invTargetExtent;
    };

    // radius scales the tent filter's tap offsets - the panel's "Radius" knob.
    struct BloomUpsamplePushConstants
    {
        glm::vec2 invSrcExtent;
        glm::vec2 invTargetExtent;
        float radius;
    };

    // Shared by DepthOfField's CoC pass and its composite pass
    struct DepthOfFieldCocPushConstants
    {
        float focusDistance;
        float aperture;
        float maxBlurTexels;
        float nearClip;
        float farClip; // 0 = no far plane
        uint32_t isLinearDepth;
        glm::vec2 invSrcExtent;
        glm::vec2 invTargetExtent;
    };

    // Shared by the near and far blur passes - both just gather over the half-res CoC buffer.
    struct DepthOfFieldBlurPushConstants
    {
        float maxBlurTexels;
        float _pad0;
        glm::vec2 invTargetExtent;
    };

    struct VelocityPushConstants
    {
        glm::mat4 model;     // 64
        glm::mat4 prevModel; // 64
    };

    struct MotionBlurPushConstants
    {
        float shutterStrength;
        uint32_t sampleCount;
        glm::vec2 invExtent;
    };
}
