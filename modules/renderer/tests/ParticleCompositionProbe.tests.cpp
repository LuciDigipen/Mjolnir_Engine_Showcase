/**
 * @file ParticleCompositionProbe.tests.cpp
 * @author sumin.park
 * @brief Projection, precision and invalid-input proof for the static probe.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/CameraMath.h>
#include <renderer/ParticleCompositionProbe.h>

#include <catch2/catch_test_macros.hpp>
#include <glm/geometric.hpp>

#include <cmath>
#include <limits>

namespace
{
    const glm::vec4 kRect{40, 20, 960, 540};
    const glm::vec2 kInvExtent{1.0f / 1040, 1.0f / 600};
    mir::WorldPosition At(double x, double z = 0)
    {
        return *mir::WorldPosition::FromDoubleMeters({x, 0, z});
    }
}

TEST_CASE("Particle probe subtracts world origins before narrowing", "[renderer][particle]")
{
    const auto projection = mir::MakeViewProjection(glm::mat4(1), {60, 0.1f, 100}, 960.0f / 540);
    const auto local = mir::BuildParticleProbeView(At(0.125), At(0, 8), projection, kRect, kInvExtent, false);
    const auto remote = mir::BuildParticleProbeView(At(1000000.125), At(1000000, 8), projection, kRect, kInvExtent, true);
    REQUIRE(local);
    REQUIRE(remote);
    CHECK(local->anchor == remote->anchor);
    CHECK(remote->anchor == glm::vec4(0.125f, 0, -8, 1));
    CHECK(remote->sampling.z == 1.0f);
    CHECK(local->sampling.z == 0.0f);
    CHECK(remote->rect == kRect);
}

TEST_CASE("Particle probe inverse projection preserves off-axis ray distance", "[renderer][particle]")
{
    for (float far : {0.0f, 100.0f})
    {
        const auto projection = mir::MakeViewProjection(glm::mat4(1), {60, 0.1f, far}, 960.0f / 540);
        const auto probe = mir::BuildParticleProbeView(At(0), At(0, 8), projection, kRect, kInvExtent, false);
        REQUIRE(probe);
        const glm::vec4 point{3, 2, -8, 1};
        const auto clip = projection * point;
        const auto reconstructed = probe->invViewProj * glm::vec4(glm::vec3(clip) / clip.w, 1);
        CHECK(glm::length(glm::vec3(reconstructed) / reconstructed.w - glm::vec3(point)) < 1e-4f);
        CHECK(glm::length(glm::vec3(point)) > 8.0f); // ray distance must not be compared to view-Z
    }
}

TEST_CASE("Particle probe rejects invalid view inputs", "[renderer][particle]")
{
    CHECK_FALSE(mir::BuildParticleProbeView(At(0), At(0), glm::mat4(0), kRect, kInvExtent, false));
    CHECK_FALSE(mir::BuildParticleProbeView(At(0), At(0), glm::mat4(1), {0, 0, 0, 540}, kInvExtent, false));
    CHECK_FALSE(mir::BuildParticleProbeView(At(0), At(0), glm::mat4(1), kRect,
        {std::numeric_limits<float>::infinity(), 1}, false));
}
