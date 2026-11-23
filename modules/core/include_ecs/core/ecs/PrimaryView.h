/**
 * @file PrimaryView.h
 * @author Quy Dong Ta
 * @brief The view viewport 0 renders this frame, as a World resource, for what draws over it, such as the game UI.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/math/WorldCoordinates.h>

#include <glm/mat4x4.hpp>

#include <cstdint>

namespace mir
{
    // The renderer writes it last in LateUpdate, from the camera, lens and aspect it then draws with
    struct PrimaryView
    {
        WorldPosition mCameraPosition;
        // relative to mCameraPosition, in Vulkan clip space: y down, reverse-Z, and w the depth in meters
        glm::mat4 mViewProjection{ 1.0f };
        // in pixels
        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
        bool mValid = false;
    };
}
