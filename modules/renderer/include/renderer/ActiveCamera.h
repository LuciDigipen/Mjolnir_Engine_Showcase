/**
 * @file ActiveCamera.h
 * @author Quy Dong Ta
 * @brief Which Camera Play renders through: the one flagged active, kept to
 *        one by ActiveCameraSystem, with a defined fallback when none is.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>
#include <core/ecs/Query.h>
#include <core/ecs/System.h>
#include <renderer/components/Camera.h>

#include <cstdint>
#include <functional>
#include <unordered_map>
#include <utility>

namespace mir
{
    class World;

    // Lowest index if more than one is flagged or null if none is.
    Entity FindActiveCamera(World &world);

    // False, if camera has no Camera.
    bool SetActiveCamera(World &world, Entity camera);

    struct PlayCamera
    {
        
        Entity mEntity = kNullEntity;
        
        bool mActive = false;
    };

    // The flagged Camera or the lowest-index one
    PlayCamera ResolvePlayCamera(World &world);

    // Runs in Edit and Play, last in LateUpdate. A camera flagged since last frame wins
    // one that arrives flagged (duplicate) loses to the camera already active.
    class ActiveCameraSystem final : public ISystem
    {
    public:
        // @p picked runs after each update, once the flags are final
        explicit ActiveCameraSystem(std::function<void(World &)> picked = {}) : mPicked(std::move(picked)) {}

        void OnStart(SystemContext &context) override;
        void OnUpdate(SystemContext &context) override;

        // A scene load or a Play restore reuses entity slots, so last frame's flags mean nothing.
        void Reset() { mWasActive.clear(); }

    private:
        Query<Camera> *mQuery = nullptr;
        std::unordered_map<uint64_t, bool> mWasActive;
        std::function<void(World &)> mPicked;
    };
}
