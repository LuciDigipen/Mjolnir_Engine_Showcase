/**
 * @file RendererCapabilities.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/RendererCapabilities.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/RendererCapabilities.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("Shared material tables need every descriptor-indexing feature, not ray tracing", "[renderer][material]")
{
    RendererFeatureSupport support{
        .descriptorIndexing = true,
        .descriptorBindingPartiallyBound = true,
        .shaderSampledImageArrayNonUniformIndexing = true,
        .rayTracing = false };

    const RendererCapabilities rasterOnly = ResolveRendererCapabilities(support);
    CHECK(rasterOnly.sharedMaterialTables);
    CHECK_FALSE(rasterOnly.rayTracing);

    support.shaderSampledImageArrayNonUniformIndexing = false;
    CHECK_FALSE(ResolveRendererCapabilities(support).sharedMaterialTables);

    support.shaderSampledImageArrayNonUniformIndexing = true;
    support.descriptorBindingPartiallyBound = false;
    CHECK_FALSE(ResolveRendererCapabilities(support).sharedMaterialTables);

    support.descriptorBindingPartiallyBound = true;
    support.descriptorIndexing = false;
    CHECK_FALSE(ResolveRendererCapabilities(support).sharedMaterialTables);

    support.descriptorIndexing = true;
    support.rayTracing = true;
    CHECK(ResolveRendererCapabilities(support).rayTracing);
}

TEST_CASE("A traced scene must leave headroom after its BLASes and the largest scratch", "[renderer]")
{
    constexpr uint64_t kMiB = 1024ull * 1024ull;
    const uint64_t exact = 300 * kMiB + 40 * kMiB + kRayTracingHeadroomBytes;
    CHECK(RayTracingSceneFits(300 * kMiB, 40 * kMiB, exact));
    CHECK_FALSE(RayTracingSceneFits(300 * kMiB, 40 * kMiB, exact - 1));
    CHECK(RayTracingSceneFits(0, 0, kRayTracingHeadroomBytes));
    CHECK_FALSE(RayTracingSceneFits(0, 0, 0));
}

TEST_CASE("Sampler anisotropy capability carries its limit", "[renderer][material]")
{
    RendererFeatureSupport support{ .samplerAnisotropy = false, .maxSamplerAnisotropy = 16.0f };
    CHECK_FALSE(ResolveRendererCapabilities(support).samplerAnisotropy);
    CHECK(ResolveRendererCapabilities(support).maxSamplerAnisotropy == 1.0f);

    support.samplerAnisotropy = true;
    const RendererCapabilities capable = ResolveRendererCapabilities(support);
    CHECK(capable.samplerAnisotropy);
    CHECK(capable.maxSamplerAnisotropy == 16.0f);
}
