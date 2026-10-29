/**
 * @file ParticleRibbonGeometry.tests.cpp
 * @author sumin.park
 * @brief Finite joins, sorting, capacity and remote-camera ribbon geometry.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <particles_gpu/ParticleRibbonGeometry.h>

#include <renderer/CameraMath.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/geometric.hpp>

#include <cmath>
#include <limits>

namespace
{
    mir::RibbonPointSubmission Point(double x, double z, uint64_t link)
    {
        return {*mir::WorldPosition::FromMeters(mir::WorldVec3(x,0,z)), double(link), 1, link, 0.5f, glm::vec4(1)};
    }
}
TEST_CASE("Ribbon geometry shares joins and rejects broken links without nonfinite vertices", "[ribbon][geometry]")
{
    mir::RibbonSubmission ribbon{{Point(0,-4,0),Point(1,-4,1),Point(1,-5,2)}};
    const auto result = mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {1,0,0});
    REQUIRE(result.size() == 12);
    for (const auto &vertex : result)
        for (int c = 0; c < 4; ++c) CHECK(std::isfinite(vertex.position[c]));
    // Both sorted segments use exactly the same pair of corners at their common join.
    CHECK(result[0].position == result[11].position);
    CHECK(result[1].position == result[10].position);
    ribbon.points[1].ribbon = 2;
    CHECK(mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {1,0,0}).empty());
    ribbon.points[1].ribbon = 1;
    ribbon.points[1].link = 9;
    CHECK(mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {1,0,0}).empty());
}
TEST_CASE("End-on ribbon geometry and duplicate samples stay finite and bounded", "[ribbon][geometry]")
{
    mir::RibbonSubmission ribbon{{Point(0,-4,0),Point(0,-5,1),Point(0,-6,2)}};
    const auto result = mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {0,0,0},6);
    REQUIRE(result.size() == 6);
    for (const auto &vertex : result)
        for (int c = 0; c < 4; ++c) CHECK(std::isfinite(vertex.position[c]));
    ribbon.points[1].position = ribbon.points[0].position;
    CHECK(mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {1,0,0}).size() == 6);
    ribbon.points[2].width = std::numeric_limits<float>::quiet_NaN();
    CHECK(mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {1,0,0}).empty());
}
TEST_CASE("Remote camera returns to ribbon history independently of current aircraft tile", "[ribbon][geometry]")
{
    if constexpr (sizeof(mir::WorldReal) == sizeof(float)) return;
    mir::RibbonSubmission ribbon{{Point(0,-4,0),Point(1,-4,1)}};
    const auto near = mir::BuildRibbonGeometry(std::span(&ribbon,1), {}, {1,0,0});
    for (auto &point : ribbon.points)
        point.position = *mir::WorldPosition::FromMeters(point.position.Meters() + mir::WorldVec3(1000000,0,0));
    const auto camera = *mir::WorldPosition::FromMeters(mir::WorldVec3(1000000,0,0));
    const auto far = mir::BuildRibbonGeometry(std::span(&ribbon,1),camera,{1,0,0});
    REQUIRE(near.size() == far.size());
    for (size_t i = 0; i < near.size(); ++i)
        CHECK(glm::length(near[i].position - far[i].position) < 0.01f);
    ribbon.points[0].textureDistance += 1000000;
    ribbon.points[1].textureDistance += 1000000;
    const auto phase = mir::BuildRibbonGeometry(std::span(&ribbon,1),camera,{1,0,0});
    REQUIRE(phase.size() == far.size());
    CHECK(phase[0].uv == far[0].uv);
}

TEST_CASE("Particle sphere culling keeps edge crossings and rejects behind-camera or distant bounds", "[particles][rendering]")
{
    const auto matrix=mir::MakeViewProjection(glm::mat4(1),mir::Camera{90,0.1f,100},1);
    CHECK(mir::ParticleSphereVisible({0,0,-4},0.5f,matrix,100));
    CHECK(mir::ParticleSphereVisible({4.1f,0,-4},0.5f,matrix,100));
    CHECK_FALSE(mir::ParticleSphereVisible({8,0,-4},0.5f,matrix,100));
    CHECK_FALSE(mir::ParticleSphereVisible({0,0,4},0.5f,matrix,100));
    CHECK_FALSE(mir::ParticleSphereVisible({0,0,-120},0.5f,matrix,100));
    CHECK_FALSE(mir::ParticleSphereVisible({0,0,-4},0.5f,matrix,2));
    CHECK_FALSE(mir::ParticleSphereVisible({0,0,-4},std::numeric_limits<float>::quiet_NaN(),matrix,100));
}
TEST_CASE("Ribbon view culling and sort mode preserve history and can retain link order", "[particles][rendering]")
{
    mir::RibbonSubmission ribbon{{Point(0,-4,0),Point(0,-5,1),Point(1,-6,2)}};
    const auto original=ribbon.points;
    const auto matrix=mir::MakeViewProjection(glm::mat4(1),mir::Camera{90,0.1f,100},1);
    auto sorted=mir::BuildRibbonGeometry(std::span(&ribbon,1),{},{1,0,0},65532,&matrix);
    REQUIRE(sorted.size()==12);
    CHECK(sorted[0].position.z==-5);
    ribbon.renderer.sort=1;
    auto unsorted=mir::BuildRibbonGeometry(std::span(&ribbon,1),{},{1,0,0},65532,&matrix);
    REQUIRE(unsorted.size()==12);
    CHECK(unsorted[0].position.z==-4);
    ribbon.renderer.maxDistance=1;
    CHECK(mir::BuildRibbonGeometry(std::span(&ribbon,1),{},{1,0,0},65532,&matrix).empty());
    for(size_t i=0;i<original.size();++i) CHECK(ribbon.points[i].position.Meters()==original[i].position.Meters());
}
