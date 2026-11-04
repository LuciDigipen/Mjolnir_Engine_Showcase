/**
 * @file RendererCapabilities.h
 * @author Jackey Cen
 * @brief Raw device features resolved into the renderer's optional paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    // What the physical device reports, before the renderer decides which paths they unlock.
    struct RendererFeatureSupport
    {
        bool descriptorIndexing = false;
        bool descriptorBindingPartiallyBound = false;
        bool shaderSampledImageArrayNonUniformIndexing = false;
        bool rayTracing = false;
        bool samplerAnisotropy = false;
        float maxSamplerAnisotropy = 1.0f;
    };

    struct RendererCapabilities
    {
        // The shared image/sampler/material-record tables are a raster feature too,
        // so they do not depend on rayTracing.
        bool sharedMaterialTables = false;
        bool rayTracing = false;
        bool samplerAnisotropy = false;
        float maxSamplerAnisotropy = 1.0f;
    };

    RendererCapabilities ResolveRendererCapabilities(const RendererFeatureSupport &support);

    // Left free after a scene's BLASes for what tracing allocates next: its images and terrain's own BLASes.
    inline constexpr uint64_t kRayTracingHeadroomBytes = 1024ull * 1024ull * 1024ull;

    // Whether building these BLASes leaves device memory for the rest of the frame. scratch is only held
    // during a build, one at a time, so the largest counts.
    constexpr bool RayTracingSceneFits(uint64_t blasBytes, uint64_t largestScratchBytes, uint64_t freeBytes)
    {
        return blasBytes + largestScratchBytes + kRayTracingHeadroomBytes <= freeBytes;
    }
}
