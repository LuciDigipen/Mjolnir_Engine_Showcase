/**
 * @file DebugView.h
 * @author Rahul Nair
 * @brief How a viewport draws the scene - shaded, wireframe or both - and
 *        whether physics colliders are drawn over it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
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

        // false: the final render with nothing over it, so the renderer records no debug work
        bool Active() const { return shading != ViewportShading::Render || colliders; }

        friend bool operator==(const DebugViewSettings &, const DebugViewSettings &) = default;
    };
}
