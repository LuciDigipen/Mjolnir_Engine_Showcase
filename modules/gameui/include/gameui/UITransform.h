/**
 * @file UITransform.h
 * @author Quy Dong Ta
 * @brief A 2D affine transform in UI units, for moving, rotating and scaling
 *        what the game UI draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <optional>

namespace mir
{
    // x' = a x + c y + tx, y' = b x + d y + ty. y is down, so a positive angle turns clockwise on screen
    struct UITransform
    {
        float a = 1.0f;
        float b = 0.0f;
        float c = 0.0f;
        float d = 1.0f;
        float tx = 0.0f;
        float ty = 0.0f;

        static UITransform Translate(float x, float y);
        static UITransform Rotate(float radians, UIPoint pivot = {});
        static UITransform Scale(float x, float y, UIPoint pivot = {});

        UIPoint Apply(UIPoint point) const;

        // none when it flattens everything onto a line or a point
        std::optional<UITransform> Inverse() const;

        // how much it scales an area, square rooted: 1 for a move or a turn
        float AreaScale() const;

        // the axis-aligned bounds of the rect once transformed
        UIRect Bounds(const UIRect &rect) const;

        bool IsIdentity() const { return *this == UITransform{}; }
        bool operator==(const UITransform &) const = default;
    };

    // inner first, then outer
    UITransform operator*(const UITransform &outer, const UITransform &inner);
} // namespace mir
