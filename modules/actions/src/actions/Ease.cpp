/**
 * @file Ease.cpp
 * @author Quy Dong Ta
 * @brief Easing curves that map an action's progress to how far along its value is.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <actions/Ease.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr float kBackOvershoot = 1.70158f;

        constexpr float kBounceScale = 7.5625f;
        constexpr float kBounceSpan = 2.75f;

        constexpr std::array<std::pair<Ease, std::string_view>, 8> kNames{ {
            { Ease::Linear, "Linear" },
            { Ease::In, "In" },
            { Ease::Out, "Out" },
            { Ease::InOut, "InOut" },
            { Ease::StrongIn, "StrongIn" },
            { Ease::StrongOut, "StrongOut" },
            { Ease::Back, "Back" },
            { Ease::Bounce, "Bounce" },
        } };

        float BounceOut(float t)
        {
            if (t < 1.0f / kBounceSpan)
                return kBounceScale * t * t;
            if (t < 2.0f / kBounceSpan)
            {
                t -= 1.5f / kBounceSpan;
                return kBounceScale * t * t + 0.75f;
            }
            if (t < 2.5f / kBounceSpan)
            {
                t -= 2.25f / kBounceSpan;
                return kBounceScale * t * t + 0.9375f;
            }
            t -= 2.625f / kBounceSpan;
            return kBounceScale * t * t + 0.984375f;
        }
    }

    float ApplyEase(Ease ease, float t)
    {
        if (!(t > 0.0f))
            return 0.0f;
        if (t >= 1.0f)
            return 1.0f;

        const float inverse = 1.0f - t;
        switch (ease)
        {
        case Ease::Linear:
            return t;
        case Ease::In:
            return t * t;
        case Ease::Out:
            return 1.0f - inverse * inverse;
        case Ease::InOut:
            return t < 0.5f ? 2.0f * t * t : 1.0f - 2.0f * inverse * inverse;
        case Ease::StrongIn:
            return t * t * t * t;
        case Ease::StrongOut:
            return 1.0f - inverse * inverse * inverse * inverse;
        case Ease::Back:
        {
            const float u = t - 1.0f;
            return 1.0f + (kBackOvershoot + 1.0f) * u * u * u + kBackOvershoot * u * u;
        }
        case Ease::Bounce:
            return BounceOut(t);
        }
        return t;
    }

    std::string_view EaseName(Ease ease)
    {
        for (const auto &[value, name] : kNames)
            if (value == ease)
                return name;
        return "Linear";
    }

    std::optional<Ease> EaseFromName(std::string_view name)
    {
        for (const auto &[value, entry] : kNames)
            if (entry == name)
                return value;
        return std::nullopt;
    }
}
