/**
 * @file PostProcessingBindings.h
 * @author Rahul Nair
 * @brief Extends the World usertype with world:set_fog/get_fog, set_hdr/get_hdr,
 *        set_bloom/get_bloom, set_depth_of_field/get_depth_of_field and
 *        set_film/get_film.
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
