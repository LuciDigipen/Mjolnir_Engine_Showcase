/**
 * @file PostProcessingBindings.h
 * @author Rahul Nair
 * @brief Extends the World usertype with world:set_fog/get_fog, set_hdr/get_hdr
 *        and set_bloom/get_bloom.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <sol/forward.hpp>

namespace mir
{
    void RegisterPostProcessingBindings(sol::state &lua);
}
