/**
 * @file ViewSubmission.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/ViewSubmission.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ViewSubmission.h>
#include "../src/renderer/vulkan/Ddgi.h"
#include "../src/renderer/vulkan/RasterSurfaces.h"
#include "../src/renderer/vulkan/SsgiTrace.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/quaternion.hpp>
#include <glm/vec4.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

namespace
{
    using mir::DrawItem;
    using mir::WorldDrawItem;
    using mir::WorldPose;
    using mir::WorldPosition;

    // Map-edge coordinates shared with terrain ADRs 0002 and 0003.
    const glm::dvec3 kEdgeA{-150000.125, 350.5, 75000.5};
    const glm::dvec3 kEdgeB{150000.625, -20.0, -75000.25};
    const glm::dvec3 kStep{0.001, -0.002, 0.003};

    WorldPosition At(const glm::dvec3 &meters)
    {
        const std::optional<WorldPosition> position = WorldPosition::FromDoubleMeters(meters);
        REQUIRE(position);
        return *position;
    }

    WorldDrawItem ItemAt(const glm::dvec3 &meters, bool transparent = false)
    {
        WorldDrawItem item;
        item.pose = WorldPose{At(meters)};
        item.transparent = transparent;
        return item;
    }

    double MaxError(const glm::dvec3 &actual, const glm::dvec3 &expected)
    {
        const glm::dvec3 error = glm::abs(actual - expected);
        return std::max({error.x, error.y, error.z});
    }

    glm::dvec3 Translation(const DrawItem &item) { return glm::dvec3(glm::vec3(item.model[3])); }

    glm::vec3 Ndc(const glm::mat4 &viewProj, const glm::mat4 &model)
    {
        const glm::vec4 clip = viewProj * model * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        return glm::vec3(clip) / clip.w;
    }

    const mir::Camera kLens{60.0f, 0.1f, 100.0f};
}

TEST_CASE("SSGI tracing bounds odd resolutions, view isolation and total GPU storage", "[renderer][ssgi]")
{
    using mir::vk::SsgiTrace;
    CHECK(SsgiTrace::MipCount({ 0, 0 }) == 0);
    CHECK(SsgiTrace::MipCount({ 1, 1 }) == 1);
    CHECK(SsgiTrace::MipCount({ 641, 359 }) == 10);
    CHECK(SsgiTrace::TraceExtent({ 641, 359 }, true).width == 321);
    CHECK(SsgiTrace::TraceExtent({ 641, 359 }, true).height == 180);
    CHECK(SsgiTrace::TraceExtent({ 641, 359 }, false).width == 641);
    VkPhysicalDeviceLimits limits{};
    limits.maxImageDimension2D = 16384;
    limits.maxPushConstantsSize = 128;
    limits.maxComputeWorkGroupInvocations = 128;
    limits.maxComputeWorkGroupSize[0] = limits.maxComputeWorkGroupSize[1] = 128;
    limits.maxComputeWorkGroupCount[0] = limits.maxComputeWorkGroupCount[1] = 65535;
    limits.maxPerStageDescriptorSampledImages = 16;
    limits.maxPerStageDescriptorStorageImages = 4;
    std::vector<VkRect2D> rects{ { {}, { 1920, 1080 } } };
    const auto fits = [&](bool half = true) { return SsgiTrace::FitsBudget(rects, half, limits); };
    CHECK(fits());
    CHECK(fits(false));
    rects[0].extent = { 3840, 2160 };
    CHECK_FALSE(fits());
    CHECK_FALSE(fits(false));
    rects[0].extent = { 2560, 1440 };
    CHECK(fits());
    CHECK_FALSE(fits(false));
    rects[0].extent = { 2000, 1200 };
    CHECK(fits(false)); // Full-resolution filtering does not allocate an upsampling scratch image.
    rects = { { {}, { 640, 360 } }, { { 640, 0 }, { 640, 360 } } };
    CHECK(fits());
    rects[1].offset.x = 639;
    CHECK_FALSE(fits());
    rects[1].offset = { 0, 360 };
    CHECK(fits());
    rects[1].offset.y = 359;
    CHECK_FALSE(fits());
    rects.resize(1);
    SECTION("zero dimensions") { rects[0].extent.width = 0; CHECK_FALSE(fits()); }
    SECTION("workgroup limit") { limits.maxComputeWorkGroupCount[0] = 79; CHECK_FALSE(fits()); }
    SECTION("storage binding limit") { limits.maxPerStageDescriptorStorageImages = 1; CHECK_FALSE(fits()); }
    SECTION("sampled binding limit") { limits.maxPerStageDescriptorSampledImages = 4; CHECK_FALSE(fits()); }
    SECTION("filter push constant limit") { limits.maxPushConstantsSize = 119; CHECK_FALSE(fits()); }
    SECTION("empty views") { rects.clear(); CHECK_FALSE(fits()); }
    SECTION("filter budget is shared across views")
    {
        rects = { { {}, { 1280, 1080 } }, { { 1280, 0 }, { 1280, 1080 } } };
        CHECK(fits(false));
        rects.push_back({ { 2560, 0 }, { 1280, 1080 } });
        CHECK_FALSE(fits(false));
    }
    SECTION("too many views") { rects.resize(17); CHECK_FALSE(fits()); }
    SECTION("large products cannot wrap the budget")
    {
        limits.maxImageDimension2D = limits.maxComputeWorkGroupCount[0] = limits.maxComputeWorkGroupCount[1] = UINT32_MAX;
        rects[0].extent = { UINT32_MAX, UINT32_MAX };
        CHECK_FALSE(fits());
    }
}

TEST_CASE("SSGI history rejects cuts and projection changes while retaining camera-relative motion", "[renderer][ssgi]")
{
    using mir::vk::SsgiTrace;
    const auto position = At(kEdgeA);
    const auto matrix = mir::MakeCameraRelativeViewProjection(WorldPose{ position }, kLens, 1.5f);
    const auto compatible = [&](const glm::mat4 &next, WorldPosition camera = At(kEdgeA))
        { return SsgiTrace::HistoryCompatible(matrix, position, next, camera, 10); };
    CHECK(compatible(matrix));
    CHECK(compatible(matrix, At(kEdgeA + glm::dvec3(0.1, 0, 0))));
    CHECK_FALSE(compatible(matrix, At(kEdgeA + glm::dvec3(20, 0, 0))));
    auto turn = glm::mat3_cast(glm::angleAxis(glm::radians(3.0f), glm::vec3(0, 1, 0)));
    CHECK(compatible(mir::MakeCameraRelativeViewProjection(WorldPose{ position, turn }, kLens, 1.5f)));
    turn = glm::mat3_cast(glm::angleAxis(glm::radians(90.0f), glm::vec3(0, 1, 0)));
    CHECK_FALSE(compatible(mir::MakeCameraRelativeViewProjection(WorldPose{ position, turn }, kLens, 1.5f)));
    CHECK_FALSE(compatible(mir::MakeCameraRelativeViewProjection(WorldPose{ position }, mir::Camera{ 70, 0.1f, 100 }, 1.5f)));
    CHECK_FALSE(compatible(mir::MakeCameraRelativeViewProjection(WorldPose{ position }, kLens, 2.0f)));
    CHECK_FALSE(compatible(mir::MakeCameraRelativeViewProjection(WorldPose{ position }, mir::Camera{ 60, 0.2f, 100 }, 1.5f)));
    CHECK_FALSE(compatible(glm::mat4(1)));
    CHECK_FALSE(compatible(glm::mat4(0)));
    auto invalid = matrix; invalid[0][0] = std::numeric_limits<float>::quiet_NaN();
    CHECK_FALSE(compatible(invalid));
}

TEST_CASE("Raster surface inputs respect attachment limits and the allocation budget", "[renderer][ssgi]")
{
    VkPhysicalDeviceLimits limits{};
    limits.maxFramebufferWidth = limits.maxFramebufferHeight = limits.maxImageDimension2D = 16384;
    limits.maxColorAttachments = limits.maxFragmentOutputAttachments = 4;
    limits.maxFragmentCombinedOutputResources = 7;
    const auto fits = [&](uint32_t width, uint32_t height)
        { return mir::vk::RasterSurfaces::FitsBudget({ width, height }, limits); };
    CHECK(fits(1920, 1080));
    CHECK(fits(2560, 1440));
    CHECK_FALSE(fits(3840, 2160));
    CHECK_FALSE(fits(0, 1080));
    CHECK_FALSE(fits(1920, 0));
    SECTION("color attachment limit") { limits.maxColorAttachments = 3; CHECK_FALSE(fits(640, 360)); }
    SECTION("fragment output limit") { limits.maxFragmentOutputAttachments = 3; CHECK_FALSE(fits(640, 360)); }
    SECTION("combined resource limit") { limits.maxFragmentCombinedOutputResources = 6; CHECK_FALSE(fits(640, 360)); }
    SECTION("framebuffer limit") { limits.maxFramebufferWidth = 639; CHECK_FALSE(fits(640, 360)); }
    SECTION("image limit") { limits.maxImageDimension2D = 639; CHECK_FALSE(fits(640, 360)); }
    SECTION("large dimensions cannot overflow the byte budget")
    {
        limits.maxFramebufferWidth = limits.maxFramebufferHeight = limits.maxImageDimension2D =
            std::numeric_limits<uint32_t>::max();
        CHECK_FALSE(fits(limits.maxFramebufferWidth, limits.maxFramebufferHeight));
    }
}

TEST_CASE("Probe batches cover the volume before enabling history and respect update budgets", "[renderer][ddgi]")
{
    for (uint32_t total : { 8u, 24u, 64u, 512u })
        for (uint32_t count : { 1u, 7u, total })
        {
            mir::vk::ProbeUpdateSchedule schedule;
            std::vector<bool> seen(total, false);
            for (uint32_t frame = 0; frame < (total + count - 1) / count; ++frame)
            {
                const auto batch = schedule.Batch(total, count);
                CHECK(batch[3] == 0);
                CHECK(schedule.SamplingWeight() == 0.0f);
                std::vector<bool> thisFrame(total, false);
                for (uint32_t dispatch = 0; dispatch < batch[1]; ++dispatch)
                {
                    const uint32_t index = (batch[0] + dispatch) % batch[2];
                    CHECK_FALSE(thisFrame[index]);
                    thisFrame[index] = seen[index] = true;
                }
                CHECK(std::count(thisFrame.begin(), thisFrame.end(), true) == count);
                schedule.Advance(total, count);
            }
            CHECK(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }));
            CHECK(schedule.Batch(total, count)[3] == 1);
            float previous = schedule.SamplingWeight();
            CHECK(previous > 0.0f);
            for (int frame = 0; frame < 8; ++frame)
            {
                schedule.Advance(total, count);
                CHECK(schedule.SamplingWeight() >= previous);
                CHECK(schedule.SamplingWeight() <= 1.0f);
                previous = schedule.SamplingWeight();
            }
            CHECK(schedule.SamplingWeight() == 1.0f);
            schedule.Reset();
            CHECK(schedule.Batch(total, count)[0] == 0);
            CHECK(schedule.Batch(total, count)[3] == 0);
            CHECK(schedule.SamplingWeight() == 0.0f);
        }
}

TEST_CASE("Probe history survives intermittent incomplete scenes and expires after prolonged gaps", "[renderer][ddgi]")
{
    mir::vk::ProbeUpdateSchedule schedule;
    schedule.Advance(64, 8);
    const auto partial = schedule.Batch(64, 8);
    schedule.SkipUpdate();
    CHECK(schedule.Batch(64, 8) == partial);
    CHECK_FALSE(schedule.Expired());
    CHECK(schedule.SamplingWeight() == 0.0f);
    for (int i = 0; i < 16; ++i) schedule.Advance(64, 8);
    REQUIRE(schedule.SamplingWeight() == 1.0f);
    for (int i = 0; i < 200; ++i)
    {
        const auto batch = schedule.Batch(64, 8);
        schedule.SkipUpdate();
        CHECK(schedule.Batch(64, 8) == batch);
        CHECK(schedule.SamplingWeight() == 1.0f);
        CHECK_FALSE(schedule.Expired());
        schedule.Advance(64, 8);
    }
    for (int i = 0; i < 8; ++i) schedule.SkipUpdate();
    CHECK(schedule.SamplingWeight() == 1.0f);
    float previous = 1.0f;
    for (int i = 0; i < 12; ++i)
    {
        schedule.SkipUpdate();
        CHECK(schedule.SamplingWeight() < previous);
        previous = schedule.SamplingWeight();
    }
    CHECK(previous > 0);
    for (int i = 0; i < 4; ++i)
    {
        schedule.Advance(64, 8);
        CHECK(schedule.SamplingWeight() > previous);
        previous = schedule.SamplingWeight();
    }
    for (int i = 0; i < 100; ++i) schedule.SkipUpdate();
    CHECK(schedule.Expired());
    CHECK(schedule.SamplingWeight() == 0.0f);
    schedule.Reset();
    CHECK_FALSE(schedule.Expired());
    CHECK(schedule.Batch(64, 8) == std::array<uint32_t, 4>{0, 8, 64, 0});
    CHECK(schedule.SamplingWeight() == 0.0f);
}

TEST_CASE("GI visibility reach is independent of probe density and placement", "[renderer][ddgi]")
{
    mir::DdgiSettings settings;
    settings.origin = { kEdgeA.x, kEdgeA.y, kEdgeA.z };
    const auto volume = mir::MakeGiTraceVolume(settings);
    REQUIRE(volume);
    CHECK(volume->origin.Meters() == At(kEdgeA).Meters());
    REQUIRE(std::isfinite(volume->maxRayDistance));
    // A small probe grid in a larger room must still see its ceiling and walls.
    settings.probeCounts = { 2, 2, 2 };
    settings.probesPerFrame = 8;
    settings.probeSpacing = 1.0f;
    settings.origin = { 0.0, 0.0, 0.0 };
    const auto near = mir::MakeGiTraceVolume(settings);
    REQUIRE(near);
    CHECK(near->maxRayDistance == volume->maxRayDistance);
    CHECK(near->maxRayDistance > 10000.0f);
    settings.probeSpacing = 0.0f;
    CHECK_FALSE(mir::MakeGiTraceVolume(settings));
}

TEST_CASE("RT submission collects resident shadow blockers once and keeps off-camera world draws", "[renderer][ddgi]")
{
    struct Source : mir::IViewDrawSource
    {
        int calls = 0;
        void Prepare(std::span<const mir::ViewCamera>) override {}
        void AppendViewDraws(size_t, std::vector<WorldDrawItem> &) override { FAIL("Expected resident collection"); }
        void AppendVolumeDraws(size_t, WorldPosition, const glm::mat4 &, std::vector<WorldDrawItem> &) override
            { FAIL("Expected resident collection"); }
        void AppendRayTracingDraws(size_t view, std::vector<WorldDrawItem> &out) override
        {
            CHECK(view == 0);
            ++calls;
            out.push_back(ItemAt(kEdgeA + glm::dvec3(0, 0, 5000)));
        }
    } source;
    mir::IViewDrawSource *sources[]{ &source };
    mir::ViewCamera camera{ 0, WorldPose{ At(kEdgeA) }, kLens, { 800, 600 },
        mir::MakeCameraRelativeViewProjection(WorldPose{ At(kEdgeA) }, kLens, 4.0f / 3.0f) };
    const std::vector<WorldDrawItem> world{ ItemAt(kEdgeA + glm::dvec3(0, 0, -5)),
        ItemAt(kEdgeA + glm::dvec3(0, 0, 20)) };
    std::vector<WorldDrawItem> collected;
    mir::CollectRayTracingWorldDraws(world, sources, 0, collected);
    CHECK(source.calls == 1);
    REQUIRE(collected.size() == 3);
    std::vector<DrawItem> relative;
    CHECK(mir::BuildViewDrawItems(collected, camera.pose.position, glm::mat4(1.0f), relative) == 0);
    REQUIRE(relative.size() == 3);
    CHECK(Translation(relative[1]).z == 20.0);
    CHECK(Translation(relative[2]).z == 5000.0);
}

TEST_CASE("A far draw keeps a millimeter offset from its viewport camera", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const std::vector<WorldDrawItem> world{ItemAt(kEdgeA + kStep)};
    std::vector<DrawItem> view;

    CHECK(mir::BuildViewDrawItems(world, At(kEdgeA), glm::mat4(1.0f), view) == 0);
    REQUIRE(view.size() == 1);
    CHECK(MaxError(Translation(view[0]), kStep) <= 1e-9);

    // narrowing both absolute positions first, as the Scene-origin path did,
    // loses the whole step
    CHECK(static_cast<float>(kEdgeA.x + kStep.x) - static_cast<float>(kEdgeA.x) == 0.0f);
}

TEST_CASE("Each viewport narrows against its own camera", "[renderer][lwc]")
{
    const glm::dvec3 nearA = kEdgeA + glm::dvec3(2.0, 0.0, -3.0);
    const glm::dvec3 nearB = kEdgeB + glm::dvec3(-1.0, 0.5, 4.0);
    const std::vector<WorldDrawItem> world{ItemAt(nearA), ItemAt(nearB)};
    const std::vector<WorldDrawItem> before = world;

    std::vector<DrawItem> fromA;
    std::vector<DrawItem> fromB;
    mir::BuildViewDrawItems(world, At(kEdgeA), glm::mat4(1.0f), fromA);
    mir::BuildViewDrawItems(world, At(kEdgeB), glm::mat4(1.0f), fromB);
    REQUIRE(fromA.size() == 2);
    REQUIRE(fromB.size() == 2);

    CHECK(MaxError(Translation(fromA[0]), glm::dvec3(2.0, 0.0, -3.0)) <= 1e-9);
    CHECK(MaxError(Translation(fromB[1]), glm::dvec3(-1.0, 0.5, 4.0)) <= 1e-9);
    // across the map, the float offset still lands within float's spacing
    CHECK(MaxError(Translation(fromA[1]), nearB - kEdgeA) <= 0.04);

    // the camera-relative value is submission-only: the world items are untouched
    for (size_t i = 0; i < world.size(); ++i)
        CHECK(world[i].pose.position.Meters() == before[i].pose.position.Meters());
}

TEST_CASE("A far camera projects like the same camera at the origin", "[renderer][lwc]")
{
    const glm::mat3 turn = glm::mat3_cast(glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f)));
    const glm::dvec3 offset{0.25, -0.5, -6.0};

    const glm::mat4 farViewProj = mir::MakeCameraRelativeViewProjection(WorldPose{At(kEdgeB), turn}, kLens, 1.5f);
    const glm::mat4 nearViewProj = mir::MakeCameraRelativeViewProjection(WorldPose{WorldPosition{}, turn}, kLens, 1.5f);
    CHECK(farViewProj == nearViewProj);

    std::vector<DrawItem> farView;
    std::vector<DrawItem> nearView;
    mir::BuildViewDrawItems(std::vector<WorldDrawItem>{ItemAt(kEdgeB + offset)}, At(kEdgeB), farViewProj, farView);
    mir::BuildViewDrawItems(std::vector<WorldDrawItem>{ItemAt(offset)}, WorldPosition{}, nearViewProj, nearView);
    REQUIRE(farView.size() == 1);
    REQUIRE(nearView.size() == 1);

    const glm::vec3 farNdc = Ndc(farViewProj, farView[0].model);
    const glm::vec3 nearNdc = Ndc(nearViewProj, nearView[0].model);
    CHECK(MaxError(glm::dvec3(farNdc), glm::dvec3(nearNdc)) <= 1e-6);
}

TEST_CASE("Far transparent draws sort back to front after the opaque ones", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // camera at A looking down -Z; two transparent draws 1 cm apart in depth,
    // well inside float's 1/64 m spacing at this X
    const WorldPose camera{At(kEdgeA)};

    std::vector<WorldDrawItem> world{
        ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, -5.00), true),
        ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, -9.00), false),
        ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, -5.01), true),
    };
    world[0].tint.r = 0.0f; // nearer
    world[2].tint.r = 2.0f; // farther

    // the order is by view depth, so it holds with or without a far plane
    for (const mir::Camera &lens : {kLens, mir::Camera{60.0f, 0.1f, 0.0f}})
    {
        const glm::mat4 viewProj = mir::MakeCameraRelativeViewProjection(camera, lens, 1.0f);

        std::vector<DrawItem> view;
        mir::BuildViewDrawItems(world, camera.position, viewProj, view);
        REQUIRE(view.size() == 3);
        CHECK_FALSE(view[0].transparent);
        CHECK(view[1].tint.r == 2.0f);
        CHECK(view[2].tint.r == 0.0f);
    }
}

TEST_CASE("A draw beyond float range of the camera is left out", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    const std::vector<WorldDrawItem> world{ItemAt(glm::dvec3(1e300, 0.0, 0.0)), ItemAt(kStep)};
    std::vector<DrawItem> view{DrawItem{}, DrawItem{}, DrawItem{}};

    CHECK(mir::BuildViewDrawItems(world, WorldPosition{}, glm::mat4(1.0f), view) == 1);
    REQUIRE(view.size() == 1);
    CHECK(MaxError(Translation(view[0]), kStep) <= 1e-9);
}

TEST_CASE("Submission carries the linear part and draw data through", "[renderer]")
{
    WorldDrawItem item = ItemAt(kEdgeB);
    item.pose.linear = glm::mat3(glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.5f));
    item.normalMatrix = glm::mat3(glm::vec3(0.5f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 2.0f));
    item.tint = glm::vec4(0.1f, 0.2f, 0.3f, 0.4f);
    const glm::vec4 deformation[2]{};
    item.deformation = deformation;

    std::vector<DrawItem> view;
    mir::BuildViewDrawItems(std::vector<WorldDrawItem>{item}, At(kEdgeB), glm::mat4(1.0f), view);
    REQUIRE(view.size() == 1);
    CHECK(glm::mat3(view[0].model) == item.pose.linear);
    CHECK(view[0].model[3] == glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    CHECK(view[0].normalMatrix == item.normalMatrix);
    CHECK(view[0].tint == item.tint);
    CHECK(view[0].deformation.data() == deformation);
    CHECK(view[0].deformation.size() == 2);
}

TEST_CASE("Neighbouring tile draws meet on their shared edge after narrowing", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    // Identity linear part, 256 m apart at the map edge.
    constexpr float kTile = 256.0f;
    constexpr float kGrid = 1.0f / 4096.0f;
    for (const float grid : {0.0f, kGrid})
    {
        INFO("translation grid " << grid);
        std::vector<WorldDrawItem> world{ItemAt(kEdgeA), ItemAt(kEdgeA + glm::dvec3(kTile, 0.0, 0.0)),
                                         ItemAt(kEdgeA + glm::dvec3(0.0, 0.0, kTile))};
        for (WorldDrawItem &item : world)
            item.translationGrid = grid;

        int crossEdgeMismatches = 0;
        int alongEdgeMismatches = 0;
        double worstTranslation = 0.0;
        for (int step = 0; step < 200; ++step)
        {
            // None on float's 1/64 m grid out there.
            const glm::dvec3 offset{-600.0 + 6.37 * step, 1.7 + 0.91 * step, 590.0 - 5.83 * step};
            std::vector<DrawItem> view;
            REQUIRE(mir::BuildViewDrawItems(world, At(kEdgeA + offset), glm::mat4(1.0f), view) == 0);
            REQUIRE(view.size() == 3);
            worstTranslation = std::max(worstTranslation, MaxError(Translation(view[0]), -offset));

            const double ulp = std::ldexp(1.0, std::ilogb(glm::length(offset) + 2.0 * kTile) - 23);
            for (int s = 0; s <= 64; ++s)
            {
                const float along = 4.0f * float(s);
                const float height = 180.0f + 0.37f * float(s);
                // The x edge of draw 0 is draw 1's x = 0, and its z edge is draw 2's z = 0.
                const glm::vec4 a = view[0].model * glm::vec4(kTile, height, along, 1.0f);
                const glm::vec4 b = view[1].model * glm::vec4(0.0f, height, along, 1.0f);
                const glm::vec4 c = view[0].model * glm::vec4(along, height, kTile, 1.0f);
                const glm::vec4 d = view[2].model * glm::vec4(along, height, 0.0f, 1.0f);
                crossEdgeMismatches += (a.y != b.y || a.z != b.z || c.x != d.x || c.y != d.y) ? 1 : 0;
                alongEdgeMismatches += (a.x != b.x ? 1 : 0) + (c.z != d.z ? 1 : 0);
                CHECK(std::abs(double(a.x) - b.x) <= ulp);
                CHECK(std::abs(double(c.z) - d.z) <= ulp);
            }
        }
        CHECK(crossEdgeMismatches == 0);
        if (grid > 0.0f)
        {
            CHECK(alongEdgeMismatches == 0);
            CHECK(worstTranslation <= kGrid / 2.0);
        }
        else
        {
            // Each translation rounds in its own binade, so plain draws disagree.
            CHECK(alongEdgeMismatches > 0);
            CHECK(worstTranslation <= 1e-4);
        }
    }
}

TEST_CASE("A translation grid only applies within float's exact range", "[renderer][lwc]")
{
    if (!mir::kLargeWorld)
        SKIP("float world positions (MIR_LARGE_WORLD=OFF) cannot hold this");

    constexpr float kGrid = 1.0f / 4096.0f;
    // 2^23 steps of 1/4096 m is 2048 m.
    std::vector<WorldDrawItem> world{ItemAt(kEdgeA + glm::dvec3(2047.3, 0.0, 0.0)),
                                     ItemAt(kEdgeA + glm::dvec3(5000.3, 0.0, 0.0))};
    for (WorldDrawItem &item : world)
        item.translationGrid = kGrid;

    std::vector<DrawItem> view;
    REQUIRE(mir::BuildViewDrawItems(world, At(kEdgeA), glm::mat4(1.0f), view) == 0);
    REQUIRE(view.size() == 2);
    const float inside = view[0].model[3].x;
    CHECK(inside == float(std::round(2047.3 / kGrid) * kGrid));
    CHECK(view[1].model[3].x == float(5000.3));
}

TEST_CASE("Debug lines narrow against the viewport camera and stay pairs", "[renderer][debug-view]")
{
    const glm::dvec3 camera{ 1000.0, 50.0, -2000.0 };
    std::vector<mir::DebugLineVertex> world{
        { At(camera + glm::dvec3(1.0, 0.0, 0.0)), 0x11u },
        { At(camera + glm::dvec3(0.0, 2.0, 0.0)), 0x22u } };

    std::vector<mir::DebugLinePoint> view;
    mir::BuildViewDebugLines(world, At(camera), view);
    REQUIRE(view.size() == 2);
    CHECK(view[0].position == glm::vec3(1.0f, 0.0f, 0.0f));
    CHECK(view[1].position == glm::vec3(0.0f, 2.0f, 0.0f));
    CHECK(view[0].color == 0x11u);
    CHECK(view[1].color == 0x22u);

    // an odd trailing vertex has no partner, so it is not a segment
    world.push_back({ At(camera), 0x33u });
    mir::BuildViewDebugLines(world, At(camera), view);
    CHECK(view.size() == 2);
}
