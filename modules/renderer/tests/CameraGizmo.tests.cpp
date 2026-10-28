/**
 * @file CameraGizmo.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the Camera component's viewport gizmo: the marker every
 *        camera draws and the frustum a selected one adds.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/ComponentRegistration.h>
#include <renderer/components/Camera.h>

#include <core/ecs/ComponentGizmo.h>
#include <core/ecs/ComponentRegistry.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <vector>

namespace
{
    struct Segment
    {
        glm::vec3 a;
        glm::vec3 b;
    };

    class RecordingCanvas final : public mir::GizmoCanvas
    {
    public:
        explicit RecordingCanvas(float aspect)
            : mAspect(aspect)
        {}

        void Line(const glm::vec3 &a, const glm::vec3 &b, uint32_t, float) const override { mLines.push_back({ a, b }); }

        void Circle(const glm::vec3 &, const glm::vec3 &, const glm::vec3 &, float, uint32_t, int) const override {}

        float Aspect() const override { return mAspect; }

        bool Touches(const glm::vec3 &point) const
        {
            return std::any_of(mLines.begin(), mLines.end(), [&](const Segment &line)
                {
                    return glm::length(line.a - point) < 1e-4f || glm::length(line.b - point) < 1e-4f;
                });
        }

        float FarthestAlongMinusZ(float originZ) const
        {
            float farthest = 0.0f;
            for (const Segment &line : mLines)
                farthest = std::max({ farthest, originZ - line.a.z, originZ - line.b.z });
            return farthest;
        }

        mutable std::vector<Segment> mLines;

    private:
        float mAspect;
    };

    const mir::ComponentOps &CameraOps()
    {
        mir::RegisterRendererComponents();
        const mir::ComponentOps *ops = mir::ComponentRegistry::Instance().Find("Camera");
        REQUIRE(ops != nullptr);
        REQUIRE(ops->mDrawGizmo != nullptr);
        return *ops;
    }

    mir::Camera Lens(float farZ)
    {
        mir::Camera lens{};
        lens.mFovYDegrees = 90.0f;
        lens.mNearZ = 1.0f;
        lens.mFarZ = farZ;
        return lens;
    }

    const glm::mat4 kAtZ5 = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, 5.0f));
}

TEST_CASE("A selected camera draws its near and far planes sized by fov and aspect", "[renderer][camera]")
{
    const mir::Camera lens = Lens(10.0f);
    RecordingCanvas canvas(2.0f);
    CameraOps().mDrawGizmo(&lens, kAtZ5, true, canvas);

    // tan(45) = 1: half-height is the distance, half-width twice that
    CHECK(canvas.Touches(glm::vec3(2.0f, 1.0f, 4.0f)));
    CHECK(canvas.Touches(glm::vec3(-2.0f, -1.0f, 4.0f)));
    CHECK(canvas.Touches(glm::vec3(20.0f, 10.0f, -5.0f)));
    CHECK(canvas.Touches(glm::vec3(-20.0f, -10.0f, -5.0f)));
    // marker (8) + near (4) + far (4) + edges (4)
    CHECK(canvas.mLines.size() == 20);
}

TEST_CASE("An unselected camera draws only its small marker", "[renderer][camera]")
{
    const mir::Camera lens = Lens(10.0f);
    RecordingCanvas canvas(1.0f);
    CameraOps().mDrawGizmo(&lens, kAtZ5, false, canvas);

    CHECK(canvas.mLines.size() == 8);
    CHECK(canvas.FarthestAlongMinusZ(5.0f) < 1.0f);
}

TEST_CASE("A lens with no far plane draws open edges", "[renderer][camera]")
{
    const mir::Camera lens = Lens(0.0f);
    RecordingCanvas canvas(1.0f);
    CameraOps().mDrawGizmo(&lens, kAtZ5, true, canvas);

    // no far rectangle
    CHECK(canvas.mLines.size() == 16);
    CHECK(canvas.FarthestAlongMinusZ(5.0f) > 1.0f);
}

TEST_CASE("The frustum ignores the entity's scale", "[renderer][camera]")
{
    const mir::Camera lens = Lens(10.0f);
    RecordingCanvas canvas(2.0f);
    CameraOps().mDrawGizmo(&lens, glm::scale(kAtZ5, glm::vec3(3.0f)), true, canvas);

    CHECK(canvas.Touches(glm::vec3(20.0f, 10.0f, -5.0f)));
}
