/**
 * @file TerrainEditMode.h
 * @author Jackey Cen
 * @brief Terrain edit mode's state, its brush settings and the ground hit under the mouse ray.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/GroundQuery.h>
#include <editor/history/DocumentId.h>
#include <renderer/CameraMath.h>

#include <functional>
#include <optional>

namespace mir
{
    enum class TerrainBrush
    {
        RaiseLower,
        Smooth,
        Flatten,
        Paint,
    };

    using TerrainGroundSampler = std::function<GroundQueryResult(WorldPosition)>;

    inline constexpr double kTerrainCursorMaxDistance = 10000.0;

    class TerrainEditMode
    {
    public:
        static constexpr float kMinRadius = 0.5f;
        static constexpr float kMaxRadius = 512.0f;
        static constexpr float kDefaultRadius = 8.0f;
        static constexpr float kDefaultStrength = 0.5f;
        static constexpr float kDefaultFalloff = 0.5f;

        bool Active() const { return mActive; }

        // Turning on is refused outside Edit on the scene document; any other state turns the mode off.
        bool Toggle(bool editing, DocumentId document);

        // Turns the mode off once the editor left Edit or a Prefab document became active.
        void Update(bool editing, DocumentId document);

        TerrainBrush Brush() const { return mBrush; }
        void SetBrush(TerrainBrush brush) { mBrush = brush; }

        float Radius() const { return mRadius; }
        void SetRadius(float radius);
        float Strength() const { return mStrength; }
        void SetStrength(float strength);
        float Falloff() const { return mFalloff; }
        void SetFalloff(float falloff);

    private:
        bool mActive = false;
        TerrainBrush mBrush = TerrainBrush::RaiseLower;
        float mRadius = kDefaultRadius;
        float mStrength = kDefaultStrength;
        float mFalloff = kDefaultFalloff;
    };

    // The first Ready ground the ray reaches within maxDistance, found by marching and refining the crossing.
    [[nodiscard]] std::optional<WorldPosition> FindTerrainCursor(const WorldRay &ray, double maxDistance,
        const TerrainGroundSampler &ground);
}
