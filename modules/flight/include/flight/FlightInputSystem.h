/**
 * @file FlightInputSystem.h
 * @author Jackey Cen
 * @brief Reads the flight actions into each player aircraft and queues its discrete commands.
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

    class FlightInputSystem final : public ISystem
    {
      public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

      private:
        Query<Aircraft> *mQuery = nullptr;
    };
}
