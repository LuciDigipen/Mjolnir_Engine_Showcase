/**
 * @file CameraModule.h
 * @author sumin.park
 * @brief Camera controller components and their system.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ModuleContext.h>

namespace mir
{
    class CameraModule
    {
    public:
        explicit CameraModule(ModuleContext context);

        CameraModule(const CameraModule &) = delete;
        CameraModule &operator=(const CameraModule &) = delete;
    };
}
