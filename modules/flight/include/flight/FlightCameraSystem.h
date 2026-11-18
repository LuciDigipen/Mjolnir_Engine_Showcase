/**
 * @file FlightCameraSystem.h
 * @author Jackey Cen
 * @brief Configures an aircraft's chase and cockpit cameras and switches between them.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Query.h>
#include <core/ecs/System.h>

namespace mir
{
    struct Aircraft;
    struct CameraController;

    class FlightCameraSystem final : public ISystem
    {
      public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

      private:
        Query<Aircraft> *mAircraft = nullptr;
        Query<CameraController> *mControllers = nullptr;
    };
}
