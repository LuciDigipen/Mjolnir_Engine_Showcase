/**
 * @file Material.tests.cpp
 * @author Sumin Park
 * @brief Tests for renderer/Material.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/Material.h>

#include <catch2/catch_test_macros.hpp>

using namespace mir;

TEST_CASE("Sampler anisotropy clamps to the device limit", "[renderer][material]")
{
    SamplerDesc desc{ .maxAnisotropy = 8.0f };
    CHECK(EffectiveAnisotropy(desc, true, 16.0f) == 8.0f);
    CHECK(EffectiveAnisotropy(desc, true, 4.0f) == 4.0f);
    CHECK(EffectiveAnisotropy(desc, true, 1.0f) == 1.0f);
    CHECK(EffectiveAnisotropy(desc, false, 16.0f) == 1.0f);

    desc.maxAnisotropy = 1.5f;
    CHECK(EffectiveAnisotropy(desc, true, 16.0f) == 2.0f);
    desc.maxAnisotropy = 1.0f;
    CHECK(EffectiveAnisotropy(desc, true, 16.0f) == 1.0f);
}

TEST_CASE("The sampler cache key includes the effective anisotropic state", "[renderer][material]")
{
    SamplerDesc desc;
    const uint64_t isotropic = SamplerCacheKey(desc, true, 16.0f);

    desc.maxAnisotropy = 1.0f;
    CHECK(SamplerCacheKey(desc, true, 16.0f) == isotropic);

    desc.maxAnisotropy = 4.0f;
    const uint64_t anisotropic4 = SamplerCacheKey(desc, true, 16.0f);
    CHECK(anisotropic4 != isotropic);

    desc.maxAnisotropy = 8.0f;
    CHECK(SamplerCacheKey(desc, true, 16.0f) != anisotropic4);

    desc.maxAnisotropy = 16.0f;
    CHECK(SamplerCacheKey(desc, true, 4.0f) == SamplerCacheKey(SamplerDesc{ .maxAnisotropy = 8.0f }, true, 4.0f));

    desc.maxAnisotropy = 4.0f;
    CHECK(SamplerCacheKey(desc, false, 16.0f) == isotropic);

    SamplerDesc mips = desc;
    mips.useMips = true;
    CHECK(SamplerCacheKey(mips, true, 16.0f) != SamplerCacheKey(desc, true, 16.0f));

    SamplerDesc wrapped = desc;
    wrapped.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(SamplerCacheKey(wrapped, true, 16.0f) != SamplerCacheKey(desc, true, 16.0f));
}
