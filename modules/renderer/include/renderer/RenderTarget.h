/**
 * @file RenderTarget.h
 * @author Quy Dong Ta
 * @brief An image a camera renders into instead of the screen, for the game UI to show.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>

namespace mir
{
    struct RenderTargetHandle
    {
        static constexpr uint32_t kNullIndex = UINT32_MAX;

        uint32_t mIndex = kNullIndex;
        uint32_t mGeneration = 0;

        constexpr bool IsNull() const { return mIndex == kNullIndex; }
        constexpr bool operator==(const RenderTargetHandle &) const = default;
    };

    inline constexpr RenderTargetHandle kNullRenderTarget{};

    // the largest side a target may have, in pixels
    inline constexpr uint32_t kMaxRenderTargetSize = 4096;
    // live at once; a frame draws fewer when its screen views take the slots
    inline constexpr uint32_t kMaxRenderTargets = 15;
}
