/**
 * @file ComponentRegistration.cpp
 * @author Quy Dong Ta
 * @brief Registers every component the camera module defines.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "camera/ComponentRegistration.h"

#include "camera/components/CameraController.h"

#include <core/ecs/ComponentRegistry.h>

namespace mir
{
    void RegisterCameraComponents()
    {
        ComponentRegistry::Instance().Register<CameraController>(kCameraControllerFields);
    }
}
