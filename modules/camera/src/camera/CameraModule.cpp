/**
 * @file CameraModule.cpp
 * @author sumin.park
 * @brief Camera controller components and their system.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <camera/CameraModule.h>

#include <camera/CameraControllerSystem.h>
#include <camera/ComponentRegistration.h>

#include <core/ecs/SystemScheduler.h>

namespace mir
{
    CameraModule::CameraModule(ModuleContext context)
    {
        RegisterCameraComponents();
        context.scheduler.AddSystem<CameraControllerSystem>(SystemPhase::LateUpdate, SystemOrder{SystemOrder::kNormal});
    }
}
