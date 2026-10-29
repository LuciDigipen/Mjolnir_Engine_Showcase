/**
 * @file RendererCapabilities.h
 * @author Jackey Cen
 * @brief Raw device features resolved into the renderer's optional paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

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
}
