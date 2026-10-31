/**
 * @file DebugView.h
 * @author Rahul Nair
 * @brief How a viewport draws the scene - shaded, wireframe or both - and
 *        whether physics colliders are drawn over it, plus the camera-relative
 *        line points the renderer draws them with.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <glm/vec3.hpp>

#include <cstdint>

namespace mir
{
    // A debug line's end, relative to its view's camera.
    struct DebugLinePoint
    {
        glm::vec3 position{ 0.0f };
        // RGBA8, R in the low byte
        uint32_t color = 0xFFFFFFFFu;
    };
    static_assert(sizeof(DebugLinePoint) == 16);

    enum class ViewportShading : uint8_t
    {
        Render,
        Wireframe,
        // the shaded scene with its wireframe drawn over it
        RenderWireframe,
    };

    struct DebugViewSettings
    {
        ViewportShading shading = ViewportShading::Render;
        bool colliders = false;
    };
}
