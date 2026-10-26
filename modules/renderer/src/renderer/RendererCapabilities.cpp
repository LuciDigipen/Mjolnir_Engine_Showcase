/**
 * @file RendererCapabilities.cpp
 * @author Sumin Park
 * @brief Resolves raw device features into optional renderer paths.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RendererCapabilities.h>

namespace mir
{
    RendererCapabilities ResolveRendererCapabilities(const RendererFeatureSupport &support)
    {
        return RendererCapabilities{
            .sharedMaterialTables = support.descriptorIndexing &&
                support.descriptorBindingPartiallyBound &&
                support.shaderSampledImageArrayNonUniformIndexing,
            .rayTracing = support.rayTracing,
            .samplerAnisotropy = support.samplerAnisotropy,
            .maxSamplerAnisotropy = support.samplerAnisotropy ? support.maxSamplerAnisotropy : 1.0f };
    }
}
