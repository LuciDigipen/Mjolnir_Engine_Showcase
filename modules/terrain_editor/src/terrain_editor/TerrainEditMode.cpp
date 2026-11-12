/**
 * @file TerrainEditMode.cpp
 * @author Jackey Cen
 * @brief Terrain edit mode's state, its brush settings and the ground hit under the mouse ray.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainEditMode.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace mir
{
    namespace
    {
        constexpr int kMarchSteps = 96;
        constexpr int kRefineSteps = 48;
        constexpr double kMinDirectionLength = 1e-12;

        struct GroundPoint
        {
            double mDistance = 0.0;
            double mHeight = 0.0;
            WorldPosition mSurface;
        };

        std::optional<WorldPosition> PointAlong(const WorldRay &ray, const glm::dvec3 &direction, double distance)
        {
            return WorldPosition::FromDoubleMeters(glm::dvec3(ray.origin.Meters()) + direction * distance);
        }

        // Surface minus the query point's own height: positive once the ray is below the ground.
        std::optional<double> GroundHeight(const GroundQueryResult &result, WorldPosition point)
        {
            if (!result.IsReady())
                return std::nullopt;
            const double height = result.surface->Meters().y - point.Meters().y;
            if (!std::isfinite(height))
                return std::nullopt;
            return height;
        }

        std::optional<WorldPosition> RefineCrossing(const WorldRay &ray, const glm::dvec3 &direction,
            const TerrainGroundSampler &ground, GroundPoint low, GroundPoint high)
        {
            for (int i = 0; i < kRefineSteps; ++i)
            {
                const double distance = (low.mDistance + high.mDistance) * 0.5;
                const std::optional<WorldPosition> point = PointAlong(ray, direction, distance);
                if (!point)
                    break;
                const GroundQueryResult result = ground(*point);
                const std::optional<double> height = GroundHeight(result, *point);
                if (!height)
                    break;

                GroundPoint middle{ distance, *height, *result.surface };
                if ((low.mHeight <= 0.0) == (middle.mHeight <= 0.0))
                    low = middle;
                else
                    high = middle;
            }
            return std::abs(low.mHeight) <= std::abs(high.mHeight) ? low.mSurface : high.mSurface;
        }

        bool FiniteNonNegative(double value)
        {
            return std::isfinite(value) && value >= 0.0;
        }
    }

    bool TerrainEditMode::Toggle(bool editing, DocumentId document)
    {
        if (mActive)
        {
            mActive = false;
            return mActive;
        }
        mActive = editing && document == kSceneDocument;
        return mActive;
    }

    void TerrainEditMode::Update(bool editing, DocumentId document)
    {
        if (mActive && (!editing || document != kSceneDocument))
            mActive = false;
    }

    void TerrainEditMode::SetRadius(float radius)
    {
        if (std::isfinite(radius))
            mRadius = std::clamp(radius, kMinRadius, kMaxRadius);
    }

    void TerrainEditMode::SetStrength(float strength)
    {
        if (std::isfinite(strength))
            mStrength = std::clamp(strength, 0.0f, 1.0f);
    }

    void TerrainEditMode::SetFalloff(float falloff)
    {
        if (std::isfinite(falloff))
            mFalloff = std::clamp(falloff, 0.0f, 1.0f);
    }

    std::optional<WorldPosition> FindTerrainCursor(const WorldRay &ray, double maxDistance,
        const TerrainGroundSampler &ground)
    {
        if (!ground || !FiniteNonNegative(maxDistance) || maxDistance == 0.0)
            return std::nullopt;

        glm::dvec3 direction(ray.direction);
        const double length = glm::length(direction);
        if (!(length > kMinDirectionLength) || !std::isfinite(length))
            return std::nullopt;
        direction /= length;

        std::optional<GroundPoint> previous;
        for (int i = 0; i <= kMarchSteps; ++i)
        {
            const double distance = maxDistance * static_cast<double>(i) / kMarchSteps;
            const std::optional<WorldPosition> point = PointAlong(ray, direction, distance);
            if (!point)
                break;

            const GroundQueryResult result = ground(*point);
            const std::optional<double> height = GroundHeight(result, *point);
            if (!height)
                continue;

            const GroundPoint current{ distance, *height, *result.surface };
            if (*height == 0.0)
                return current.mSurface;
            if (previous && (previous->mHeight <= 0.0) != (current.mHeight <= 0.0))
                return RefineCrossing(ray, direction, ground, *previous, current);
            previous = current;
        }
        return std::nullopt;
    }
}
