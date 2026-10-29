/**
 * @file ColliderLines.cpp
 * @author Rahul Nair
 * @brief A collider's shape in world space, and the debug line segments that
 *        outline it - boxes, spheres, capsules and height fields.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <physics/ColliderLines.h>

#include <glm/gtc/constants.hpp>

#include <cmath>
#include <optional>

namespace mir
{
    namespace
    {
        constexpr uint32_t kTriggerColor = PackDebugColor(0.25f, 0.9f, 1.0f, 0.8f);
        constexpr uint32_t kSleepingColor = PackDebugColor(0.55f, 0.55f, 0.55f, 1.0f);
        constexpr uint32_t kDynamicColor = PackDebugColor(0.3f, 1.0f, 0.35f, 1.0f);
        // static and kinematic: nothing simulates them, they only get in the way
        constexpr uint32_t kFixedColor = PackDebugColor(0.35f, 0.6f, 1.0f, 1.0f);

        std::optional<WorldPosition> ToWorld(const ColliderView &view, const glm::vec3 &local)
        {
            const std::optional<WorldOffset> offset = WorldOffset::FromMeters(WorldVec3(view.mRotation * local));
            return offset ? TryTranslateWorld(view.mPosition, *offset) : std::nullopt;
        }

        void Segment(const ColliderView &view, const glm::vec3 &a, const glm::vec3 &b, uint32_t color,
            std::vector<DebugLineVertex> &out)
        {
            const std::optional<WorldPosition> from = ToWorld(view, a);
            const std::optional<WorldPosition> to = ToWorld(view, b);
            if (!from || !to)
                return;
            out.push_back({ *from, color });
            out.push_back({ *to, color });
        }

        // An arc around center in the plane of the unit axes u and w, from angle start to end.
        void Arc(const ColliderView &view, const glm::vec3 &center, const glm::vec3 &u, const glm::vec3 &w,
            float radius, float start, float end, uint32_t segments, uint32_t color, std::vector<DebugLineVertex> &out)
        {
            const auto at = [&](uint32_t i)
            {
                const float angle = start + (end - start) * static_cast<float>(i) / static_cast<float>(segments);
                return center + radius * (std::cos(angle) * u + std::sin(angle) * w);
            };
            for (uint32_t i = 0; i < segments; ++i)
                Segment(view, at(i), at(i + 1), color, out);
        }

        void AppendBox(const ColliderView &view, uint32_t color, std::vector<DebugLineVertex> &out)
        {
            const glm::vec3 h = view.mHalfExtent;
            const glm::vec3 corners[8] = {
                { -h.x, -h.y, -h.z },{ h.x, -h.y, -h.z },{ h.x, h.y, -h.z },{ -h.x, h.y, -h.z },
                { -h.x, -h.y, h.z },{ h.x, -h.y, h.z },{ h.x, h.y, h.z },{ -h.x, h.y, h.z } };
            static constexpr int kEdges[12][2] = {
                { 0, 1 },{ 1, 2 },{ 2, 3 },{ 3, 0 },{ 4, 5 },{ 5, 6 },
                { 6, 7 },{ 7, 4 },{ 0, 4 },{ 1, 5 },{ 2, 6 },{ 3, 7 } };
            for (const auto &edge : kEdges)
                Segment(view, corners[edge[0]], corners[edge[1]], color, out);
        }

        void AppendSphere(const ColliderView &view, uint32_t color, std::vector<DebugLineVertex> &out)
        {
            constexpr glm::vec3 x{ 1.0f, 0.0f, 0.0f }, y{ 0.0f, 1.0f, 0.0f }, z{ 0.0f, 0.0f, 1.0f };
            const float full = glm::two_pi<float>();
            Arc(view, glm::vec3(0.0f), x, y, view.mRadius, 0.0f, full, kColliderCircleSegments, color, out);
            Arc(view, glm::vec3(0.0f), y, z, view.mRadius, 0.0f, full, kColliderCircleSegments, color, out);
            Arc(view, glm::vec3(0.0f), z, x, view.mRadius, 0.0f, full, kColliderCircleSegments, color, out);
        }

        void AppendCapsule(const ColliderView &view, uint32_t color, std::vector<DebugLineVertex> &out)
        {
            constexpr glm::vec3 x{ 1.0f, 0.0f, 0.0f }, y{ 0.0f, 1.0f, 0.0f }, z{ 0.0f, 0.0f, 1.0f };
            const float r = view.mRadius;
            const glm::vec3 top = y * view.mCylinderHalfHeight;
            const glm::vec3 bottom = -top;
            const float full = glm::two_pi<float>();
            const float half = glm::pi<float>();
            const uint32_t halfSegments = kColliderCircleSegments / 2;

            // the rims where the caps meet the cylinder
            Arc(view, top, x, z, r, 0.0f, full, kColliderCircleSegments, color, out);
            Arc(view, bottom, x, z, r, 0.0f, full, kColliderCircleSegments, color, out);

            for (const glm::vec3 &side : { x, -x, z, -z })
                Segment(view, top + side * r, bottom + side * r, color, out);

            // each cap as two half circles across it
            Arc(view, top, x, y, r, 0.0f, half, halfSegments, color, out);
            Arc(view, top, z, y, r, 0.0f, half, halfSegments, color, out);
            Arc(view, bottom, x, -y, r, 0.0f, half, halfSegments, color, out);
            Arc(view, bottom, z, -y, r, 0.0f, half, halfSegments, color, out);
        }

        void AppendHeightField(const ColliderView &view, uint32_t color, std::vector<DebugLineVertex> &out)
        {
            const uint32_t size = view.mGridSize;
            if (size < 2 || view.mHeightGrid.size() < static_cast<size_t>(size) * size)
                return;

            const auto point = [&](uint32_t x, uint32_t z) -> const glm::vec3 & { return view.mHeightGrid[z * size + x]; };
            const auto edge = [&](const glm::vec3 &a, const glm::vec3 &b)
            {
                if (!std::isnan(a.y) && !std::isnan(b.y))
                    Segment(view, a, b, color, out);
            };

            for (uint32_t z = 0; z < size; ++z)
            {
                for (uint32_t x = 0; x < size; ++x)
                {
                    if (x + 1 < size)
                        edge(point(x, z), point(x + 1, z));
                    if (z + 1 < size)
                        edge(point(x, z), point(x, z + 1));
                }
            }
        }
    }

    uint32_t ColliderColor(const ColliderView &view)
    {
        if (view.mTrigger)
            return kTriggerColor;
        if (view.mSleeping)
            return kSleepingColor;
        return view.mMotion == PhysicsMotion::Dynamic ? kDynamicColor : kFixedColor;
    }

    void AppendColliderLines(const ColliderView &view, std::vector<DebugLineVertex> &out)
    {
        const uint32_t color = ColliderColor(view);
        if (view.mHeightField)
        {
            AppendHeightField(view, color, out);
            return;
        }

        switch (view.mShape)
        {
        case PhysicsShape::Box:
            AppendBox(view, color, out);
            break;
        case PhysicsShape::Sphere:
            AppendSphere(view, color, out);
            break;
        case PhysicsShape::Capsule:
            AppendCapsule(view, color, out);
            break;
        }
    }
}
