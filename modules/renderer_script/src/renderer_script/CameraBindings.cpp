/**
 * @file CameraBindings.cpp
 * @author Quy Dong Ta
 * @brief Extends the World usertype with world:get_active_camera,
 *        world:set_active_camera and world:cut_camera.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer_script/CameraBindings.h>

#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <renderer/ActiveCamera.h>
#include <renderer/CameraCut.h>

#include <sol/sol.hpp>

namespace mir
{
    namespace
    {
        sol::object WorldGetActiveCamera(World &world, sol::this_state state)
        {
            const Entity camera = FindActiveCamera(world);
            if (camera.IsNull())
                return sol::nil;
            return sol::make_object(sol::state_view(state), camera);
        }

        // Clears the flag on every other Camera now, so get_active_camera agrees this frame.
        bool WorldSetActiveCamera(World &world, Entity camera)
        {
            if (SetActiveCamera(world, camera))
                return true;
            MIR_LOG_ERROR("script: world:set_active_camera - entity {} has no Camera", camera.mIndex);
            return false;
        }

        bool WorldCutCamera(World &world, Entity camera)
        {
            if (CutCamera(world, camera))
                return true;
            MIR_LOG_ERROR("script: world:cut_camera - entity {} has no Camera", camera.mIndex);
            return false;
        }
    }

    void RegisterCameraBindings(sol::state &lua)
    {
        sol::table worldType = lua["World"];
        worldType.set_function("get_active_camera", &WorldGetActiveCamera);
        worldType.set_function("set_active_camera", &WorldSetActiveCamera);
        worldType.set_function("cut_camera", &WorldCutCamera);
    }
}
