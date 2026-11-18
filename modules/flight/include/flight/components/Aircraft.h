/**
 * @file Aircraft.h
 * @author Jackey Cen
 * @brief Marks an entity as an aircraft flown by the flight pipeline.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <flight/AircraftRuntime.h>

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mir
{
    enum class AerodynamicTier : int32_t
    {
        L1,
        L2,
        L3,
    };

    inline constexpr std::string_view kAerodynamicTierNames[] = { "L1", "L2", "L3" };

    struct Aircraft
    {
        // Relative to the game's assets root.
        char config[kFieldStringCapacity] = {};
        AerodynamicTier tier = AerodynamicTier::L2;

        AircraftRuntime runtime;
    };

    MIR_ASSERT_COMPONENT(Aircraft);

    inline constexpr FieldDesc kAircraftFields[] = {
        { .mName = "config", .mKind = FieldKind::String, .mOffset = offsetof(Aircraft, config) },
        { .mName = "tier", .mKind = FieldKind::Int, .mOffset = offsetof(Aircraft, tier),
         .mWidget = FieldWidget::Enum, .mEnumNames = kAerodynamicTierNames },
    };

    // runtime is this run's live state, never saved.
    inline constexpr TransientField kAircraftTransient[] = {
        { offsetof(Aircraft, runtime), sizeof(Aircraft::runtime) },
    };
}
