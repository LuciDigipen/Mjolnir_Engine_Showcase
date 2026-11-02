/**
 * @file CoreModule.h
 * @author sumin.park
 * @brief Core's components, ECS resources and transform propagation.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "ModuleContext.h"
#include "core/Registration.h"

namespace mir
{
    class CommandBuffer;

    // The first module: everything else assumes core's components and resources exist
    class CoreModule
    {
    public:
        // commands is the buffer App flushes each phase; FrameCommands publishes it
        CoreModule(ModuleContext context, CommandBuffer &commands);

        CoreModule(const CoreModule &) = delete;
        CoreModule &operator=(const CoreModule &) = delete;

    private:
        Registration mSceneLoad;
    };
}
