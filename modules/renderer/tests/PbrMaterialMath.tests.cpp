#include <glm/glm.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    using float3 = glm::vec3;
    using glm::max;
    using glm::min;

#include "../../../assets/shaders/PbrMaterialMath.slang"
}

TEST_CASE("PBR diffuse mixes metals after dielectric Fresnel", "[renderer][material][pbr]")
{
    const auto fresnel = PbrDielectricFresnel(float3(0.04f), 1.0f, 1.0f);
    CHECK(PbrDiffuseColor(float3(0.8f), 0.5f, fresnel).x == Catch::Approx(0.384f));
    CHECK(PbrDiffuseColor(float3(0.8f), 0.0f, fresnel).x == Catch::Approx(0.768f));
    CHECK(PbrDiffuseColor(float3(0.8f), 1.0f, fresnel) == float3(0));
    CHECK(PbrDiffuseColor(float3(0.8f), 0.5f, PbrDielectricFresnel(float3(0), 0.0f, 0.0f)).x ==
          Catch::Approx(0.4f));
    CHECK(PbrDiffuseColor(float3(0.8f), 0.5f, PbrDielectricFresnel(float3(0.04f), 1.0f, 0.0f)) == float3(0));
}

TEST_CASE("Surface tint changes metallic reflectance and preserves dielectric reflectance", "[renderer][material][pbr]")
{
    const float3 tintedAlbedo(1, 0, 0);
    const float3 dielectric(0.04f);
    CHECK(PbrMixedF0(dielectric, tintedAlbedo, 1.0f) == tintedAlbedo);
    CHECK(PbrMixedF0(dielectric, tintedAlbedo, 0.0f) == dielectric);
    const auto mixed = PbrMixedF0(dielectric, tintedAlbedo, 0.5f);
    CHECK(mixed.x == Catch::Approx(0.52f));
    CHECK(mixed.y == Catch::Approx(0.02f));
    CHECK(mixed.z == Catch::Approx(0.02f));
}
