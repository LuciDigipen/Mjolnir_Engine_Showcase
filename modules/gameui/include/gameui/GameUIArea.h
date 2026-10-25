/**
 * @file GameUIArea.h
 * @author sumin.park
 * @brief Where this frame's game UI goes, as a World resource.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

namespace mir
{
    // The editor's drawer writes its scene viewport here each frame, before the game
    // UI draws; without it the game UI fills the window
    struct GameUIArea
    {
        UIRect mRect;
        bool mSet = false;
    };
}
