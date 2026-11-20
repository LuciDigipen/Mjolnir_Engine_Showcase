/**
 * @file ActionBindings.h
 * @author Quy Dong Ta
 * @brief Registers the ActionScheduler's Lua calls on the World usertype.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterActionBindings(sol::state &lua);
}
