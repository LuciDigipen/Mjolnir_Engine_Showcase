/**
 * @file CameraBindings.h
 * @author Quy Dong Ta
 * @brief Extends the World usertype with world:get_active_camera,
 *        world:set_active_camera and world:cut_camera.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterCameraBindings(sol::state &lua);
}
