/**
 * @file ActionsModule.h
 * @author Quy Dong Ta
 * @brief Owns the ActionScheduler resource, its system and its clocks.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/ecs/ModuleContext.h>

namespace mir
{
    class ActionScheduler;

    // Ticks the Game clock in ActionSystem and the UI clock at FrameBegin, and clears everything at PlayEnd
    class ActionsModule
    {
    public:
        explicit ActionsModule(ModuleContext context);

        ActionsModule(const ActionsModule &) = delete;
        ActionsModule &operator=(const ActionsModule &) = delete;

        ActionScheduler &Scheduler() { return mScheduler; }

    private:
        ActionScheduler &mScheduler;

        // last, unregister before anything above is destroyed
        Registration mUIClock;
        Registration mPlayEnd;
    };
}
