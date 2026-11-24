/**
 * @file RenderTexture.h
 * @author Quy Dong Ta
 * @brief Makes a Camera render into its own image instead of the screen, for the game UI to show.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cstddef>
#include <cstdint>

namespace mir
{
    // Needs a Camera and a WorldTransform on the same entity. Such a camera never becomes the one Play renders
    // through, and its active flag is ignored
    struct RenderTexture
    {
        // in pixels; RenderSystem keeps each side within 1 to kMaxRenderTargetSize
        int32_t mWidth = 256;
        int32_t mHeight = 256;
    };

    MIR_ASSERT_COMPONENT(RenderTexture);

    inline constexpr FieldDesc kRenderTextureFields[] = {
        { "width", FieldKind::Int, offsetof(RenderTexture, mWidth) },
        { "height", FieldKind::Int, offsetof(RenderTexture, mHeight) },
    };
}
