/**
 * @file FlightTouchdownSystem.h
 * @author Jackey Cen
 * @brief Publishes the frame's touchdown events once per frame.
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

    class FlightTouchdownSystem final : public ISystem
    {
      public:
        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

      private:
        Query<Aircraft> *mQuery = nullptr;
    };
}
