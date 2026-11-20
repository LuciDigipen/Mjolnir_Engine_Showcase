/**
 * @file GameUIArea.h
 * @author sumin.park
 * @brief Where this frame's game UI goes, and whether it has the pointer, as World resources.
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
        // an editor tool over the viewport, such as the gizmo, has the pointer, so the game UI does not get it
        bool mToolsHavePointer = false;
    };

    // Written at the end of each game UI frame: a widget is under the pointer or held by it, so a click there is
    // the game UI's and the editor does not pick through it
    struct GameUIPointer
    {
        bool mOverWidget = false;
    };
}
