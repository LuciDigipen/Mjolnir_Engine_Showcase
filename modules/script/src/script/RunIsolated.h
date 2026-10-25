/**
 * @file RunIsolated.h
 * @author sumin.park
 * @brief Runs one script callback so a throw cannot take the other scripts down.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>
#include <core/log/Log.h>

#include <exception>

namespace mir
{
    template <typename Fn>
    void RunIsolated(Entity entity, const char *callback, Fn &&fn)
    {
        try
        {
            fn();
        }
        catch (const std::exception &e)
        {
            MIR_LOG_ERROR("script: entity {} threw in {}: {}", entity.mIndex, callback, e.what());
        }
    }
}
