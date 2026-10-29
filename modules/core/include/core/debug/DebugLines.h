/**
 * @file DebugLines.h
 * @author Rahul Nair
 * @brief World resource carrying one frame of debug line segments in world
 *        space - filled by producers such as physics, drawn by the renderer.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <cstdint>
#include <vector>

namespace mir
{
    struct DebugLineVertex
    {
        WorldPosition position;
        // RGBA8, R in the low byte
        uint32_t color = 0xFFFFFFFFu;
    };

    struct DebugLines
    {
        // Set by the renderer while a view shows colliders; producers leave the lines empty otherwise,
        // so nothing is built while no view wants them.
        bool mCollidersWanted = false;
        // Vertex pairs, one segment each. Rebuilt every frame.
        std::vector<DebugLineVertex> mColliders;
    };

    constexpr uint32_t PackDebugColor(float r, float g, float b, float a)
    {
        const auto channel = [](float value)
        {
            const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
            return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
        };
        return channel(r) | channel(g) << 8 | channel(b) << 16 | channel(a) << 24;
    }
}
