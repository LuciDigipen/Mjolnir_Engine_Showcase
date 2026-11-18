/**
 * @file AircraftConfigSystem.h
 * @author Jackey Cen
 * @brief Applies an aircraft's configuration to its RigidBody before the physics sync.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftConfig.h>
#include <flight/AircraftConfigStore.h>

#include <core/ecs/Query.h>
#include <core/ecs/System.h>

namespace mir
{
    struct Aircraft;
    struct RigidBody;

    void WriteMassProperties(const AircraftConfig &config, RigidBody &rigidBody);

    class AircraftConfigSystem final : public ISystem
    {
      public:
        explicit AircraftConfigSystem(AircraftConfigStore::Source source);

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

      private:
        AircraftConfigStore mStore;
        Query<Aircraft, RigidBody> *mQuery = nullptr;
    };
}
