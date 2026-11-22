/**
 * @file UITransform.cpp
 * @author Quy Dong Ta
 * @brief A 2D affine transform in UI units, for moving, rotating and scaling
 *        what the game UI draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UITransform.h>

#include <algorithm>
#include <cmath>

namespace mir
{
    UITransform UITransform::Translate(float x, float y)
    {
        return UITransform{.tx = x, .ty = y};
    }

    UITransform UITransform::Rotate(float radians, UIPoint pivot)
    {
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        return Translate(pivot.x, pivot.y) * UITransform{cosine, sine, -sine, cosine, 0.0f, 0.0f} *
               Translate(-pivot.x, -pivot.y);
    }

    UITransform UITransform::Scale(float x, float y, UIPoint pivot)
    {
        return Translate(pivot.x, pivot.y) * UITransform{.a = x, .d = y} * Translate(-pivot.x, -pivot.y);
    }

    UIPoint UITransform::Apply(UIPoint point) const
    {
        return UIPoint{a * point.x + c * point.y + tx, b * point.x + d * point.y + ty};
    }

    std::optional<UITransform> UITransform::Inverse() const
    {
        const float determinant = a * d - b * c;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12f)
            return std::nullopt;

        const float inverse = 1.0f / determinant;
        UITransform result{d * inverse, -b * inverse, -c * inverse, a * inverse, 0.0f, 0.0f};
        result.tx = -(result.a * tx + result.c * ty);
        result.ty = -(result.b * tx + result.d * ty);
        return result;
    }

    float UITransform::AreaScale() const
    {
        return std::sqrt(std::abs(a * d - b * c));
    }

    UIRect UITransform::Bounds(const UIRect &rect) const
    {
        if (IsIdentity())
            return rect;

        const UIPoint corners[4] = {Apply({rect.x, rect.y}), Apply({rect.Right(), rect.y}),
                                    Apply({rect.x, rect.Bottom()}), Apply({rect.Right(), rect.Bottom()})};
        float left = corners[0].x;
        float right = corners[0].x;
        float top = corners[0].y;
        float bottom = corners[0].y;
        for (const UIPoint &corner : corners)
        {
            left = std::min(left, corner.x);
            right = std::max(right, corner.x);
            top = std::min(top, corner.y);
            bottom = std::max(bottom, corner.y);
        }
        return UIRect{left, top, right - left, bottom - top};
    }

    UITransform operator*(const UITransform &outer, const UITransform &inner)
    {
        return UITransform{outer.a * inner.a + outer.c * inner.b,
                           outer.b * inner.a + outer.d * inner.b,
                           outer.a * inner.c + outer.c * inner.d,
                           outer.b * inner.c + outer.d * inner.d,
                           outer.a * inner.tx + outer.c * inner.ty + outer.tx,
                           outer.b * inner.tx + outer.d * inner.ty + outer.ty};
    }
} // namespace mir
