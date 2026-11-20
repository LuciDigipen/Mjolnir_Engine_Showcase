/**
 * @file Ease.h
 * @author Quy Dong Ta
 * @brief Easing curves that map an action's progress to how far along its value is.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace mir
{
    // In starts slow, Out ends slow. Back overshoots past 1 before settling
    enum class Ease : uint8_t
    {
        Linear,
        In,
        Out,
        InOut,
        StrongIn,
        StrongOut,
        Back,
        Bounce,
    };

    // t is clamped to [0, 1], and every curve gives 0 at 0 and 1 at 1
    float ApplyEase(Ease ease, float t);

    std::string_view EaseName(Ease ease);

    // the names EaseName gives, exactly
    std::optional<Ease> EaseFromName(std::string_view name);
}
