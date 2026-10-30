/**
 * @file RayFacing.tests.cpp
 * @author Jackey Cen
 * @brief Tests for renderer/RayFacing.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "../src/renderer/vulkan/RayFacing.h"

#include <catch2/catch_test_macros.hpp>

#include <glm/gtc/matrix_transform.hpp>

using namespace mir::vk;

namespace
{
    bool RasterFront(bool hitKindFront, bool correction)
    {
        return hitKindFront != correction;
    }
}

TEST_CASE("Ray tracing front-face correction matches raster semantics", "[renderer][material][rt]")
{
    const glm::mat4 normal = glm::mat4(1.0f);
    const glm::mat4 mirrored = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));

    CHECK_FALSE(RtRasterFacingCorrection(VK_FRONT_FACE_COUNTER_CLOCKWISE, normal, 0));
    CHECK_FALSE(RtRasterFacingCorrection(VK_FRONT_FACE_CLOCKWISE, normal,
        VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR));
    CHECK(RtRasterFacingCorrection(VK_FRONT_FACE_COUNTER_CLOCKWISE, normal,
        VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR));
    CHECK(RtRasterFacingCorrection(VK_FRONT_FACE_COUNTER_CLOCKWISE, mirrored,
        VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR));

    CHECK(RasterFront(true, RtRasterFacingCorrection(
        VK_FRONT_FACE_COUNTER_CLOCKWISE, normal, 0)));
    CHECK_FALSE(RasterFront(true, RtRasterFacingCorrection(
        VK_FRONT_FACE_COUNTER_CLOCKWISE, normal, VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR)));
    CHECK(RasterFront(false, RtRasterFacingCorrection(
        VK_FRONT_FACE_COUNTER_CLOCKWISE, normal, VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR)));
    CHECK(RasterFront(false, RtRasterFacingCorrection(VK_FRONT_FACE_CLOCKWISE, normal,
        VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR)));
}

TEST_CASE("Legacy triangle facing flags keep their culling behavior", "[renderer][material][rt]")
{
    const glm::mat4 normal = glm::mat4(1.0f);
    const glm::mat4 mirrored = glm::scale(glm::mat4(1.0f), glm::vec3(-1.0f, 1.0f, 1.0f));

    CHECK(RtTriangleFacingFlags(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, normal) ==
          VkGeometryInstanceFlagsKHR(0));
    CHECK(RtTriangleFacingFlags(VK_CULL_MODE_FRONT_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, normal) ==
          VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR);
    CHECK(RtTriangleFacingFlags(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, normal) ==
          VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR);
    CHECK_FALSE(RtTriangleFacingFlags(VK_CULL_MODE_FRONT_AND_BACK, VK_FRONT_FACE_COUNTER_CLOCKWISE, normal)
                    .has_value());
    CHECK(RtTriangleFacingFlags(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_COUNTER_CLOCKWISE, mirrored) ==
          VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR);
}
