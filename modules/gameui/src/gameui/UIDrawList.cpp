/**
 * @file UIDrawList.cpp
 * @author Quy Dong Ta
 * @brief Records rects, shapes, textures, nine-sliced sprites and text under
 *        clip, opacity and transform stacks, in coordinates local to the UI
 *        area, for the backend to replay at the end of a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>

#include <core/log/Assert.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        constexpr float kTurn = 2.0f * std::numbers::pi_v<float>;

        // any more than this is basically just a solid line, dashes too small to see
        constexpr float kMostDashes = 4096.0f;

        UIRect Inflate(const UIRect &rect, float by)
        {
            return UIRect{rect.x - by, rect.y - by, rect.width + 2.0f * by, rect.height + 2.0f * by};
        }

        UIRect PointBounds(std::span<const UIPoint> points)
        {
            float left = points[0].x;
            float right = points[0].x;
            float top = points[0].y;
            float bottom = points[0].y;
            for (const UIPoint &point : points)
            {
                left = std::min(left, point.x);
                right = std::max(right, point.x);
                top = std::min(top, point.y);
                bottom = std::max(bottom, point.y);
            }
            return UIRect{left, top, right - left, bottom - top};
        }

        UIRect CircleBounds(UIPoint center, float radius)
        {
            return UIRect{center.x - radius, center.y - radius, 2.0f * radius, 2.0f * radius};
        }

        bool IsFullTurn(float from, float to)
        {
            return std::abs(to - from) >= kTurn - 1e-4f;
        }

        // chords about 2 units long, a closed circle leaves out the end, which is its start
        std::vector<UIPoint> ArcPoints(UIPoint center, float radius, float from, float to, bool withEnd)
        {
            const float chords = std::ceil(std::abs(to - from) * radius / 2.0f);
            const int segments = static_cast<int>(std::clamp(chords, 4.0f, 1024.0f));
            std::vector<UIPoint> points;
            points.reserve(static_cast<size_t>(segments) + 1);
            for (int i = 0; i < segments + (withEnd ? 1 : 0); ++i)
            {
                const float angle = from + (to - from) * static_cast<float>(i) / static_cast<float>(segments);
                points.push_back(UIPoint{center.x + radius * std::cos(angle), center.y + radius * std::sin(angle)});
            }
            return points;
        }

        std::vector<UIPoint> RectPath(const UIRect &rect, float rounding)
        {
            const float radius = std::clamp(rounding, 0.0f, std::min(rect.width, rect.height) * 0.5f);
            if (radius <= 0.0f)
                return {{rect.x, rect.y}, {rect.Right(), rect.y}, {rect.Right(), rect.Bottom()}, {rect.x, rect.Bottom()}};

            constexpr float kQuarter = kTurn * 0.25f;
            const std::pair<UIPoint, float> corners[] = {
                {{rect.x + radius, rect.y + radius}, 2.0f * kQuarter},
                {{rect.Right() - radius, rect.y + radius}, 3.0f * kQuarter},
                {{rect.Right() - radius, rect.Bottom() - radius}, 0.0f},
                {{rect.x + radius, rect.Bottom() - radius}, kQuarter},
            };
            std::vector<UIPoint> path;
            for (const auto &[center, start] : corners)
            {
                const std::vector<UIPoint> corner = ArcPoints(center, radius, start, start + kQuarter, true);
                path.insert(path.end(), corner.begin(), corner.end());
            }
            return path;
        }

        // the pieces of the path its dashes cover, or none when they are too many to draw
        std::optional<std::vector<std::vector<UIPoint>>> Dashes(std::span<const UIPoint> path, bool closed,
                                                               const UIStroke &stroke)
        {
            std::vector<UIPoint> points(path.begin(), path.end());
            if (closed)
                points.push_back(path[0]);

            float length = 0.0f;
            for (size_t i = 1; i < points.size(); ++i)
                length += std::hypot(points[i].x - points[i - 1].x, points[i].y - points[i - 1].y);

            const float period = stroke.mDash + stroke.mGap;
            if (!(length / period <= kMostDashes))
                return std::nullopt;

            float phase = std::fmod(stroke.mDashOffset, period);
            if (phase < 0.0f)
                phase += period;
            bool inDash = phase < stroke.mDash;
            float remaining = inDash ? stroke.mDash - phase : period - phase;

            std::vector<std::vector<UIPoint>> pieces;
            std::vector<UIPoint> piece;
            if (inDash)
                piece.push_back(points[0]);

            for (size_t i = 1; i < points.size(); ++i)
            {
                const UIPoint from = points[i - 1];
                const UIPoint to = points[i];
                const float segment = std::hypot(to.x - from.x, to.y - from.y);
                if (segment <= 0.0f)
                    continue;

                float along = 0.0f;
                while (segment - along > remaining)
                {
                    along += remaining;
                    const float t = along / segment;
                    const UIPoint point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t};
                    if (inDash)
                    {
                        piece.push_back(point);
                        pieces.push_back(std::move(piece));
                        piece.clear();
                        remaining = stroke.mGap;
                    }
                    else
                    {
                        piece.push_back(point);
                        remaining = stroke.mDash;
                    }
                    inDash = !inDash;
                }
                remaining -= segment - along;
                if (inDash)
                    piece.push_back(to);
            }
            if (inDash && piece.size() >= 2)
                pieces.push_back(std::move(piece));
            return pieces;
        }
    } // namespace

    void UIDrawList::Reset(float width, float height)
    {
        mCommands.clear();
        mTextBuffer.clear();
        mPoints.clear();
        mGradients.clear();
        mClipStack.clear();
        mClipStack.push_back(UIRect{0.0f, 0.0f, std::max(width, 0.0f), std::max(height, 0.0f)});
        mOpacityStack.clear();
        mOpacityStack.push_back(1.0f);
        mTransforms.assign(1, UITransform{});
        mTransformStack.assign(1, 0);
    }

    bool UIDrawList::Begin(UIDrawCommand &command, const UIRect &bounds, UIColor color, const UIGradient &gradient)
    {
        const UIRect clip = CurrentClip();
        if (Intersect(CurrentTransform().Bounds(bounds), clip).IsEmpty())
            return false;

        command.mClip = clip;
        command.mColor = Faded(color);
        command.mTransform = mTransformStack.empty() ? 0 : mTransformStack.back();
        if (gradient.IsSet())
        {
            mGradients.push_back(gradient);
            command.mGradient = static_cast<uint32_t>(mGradients.size());
        }
        return true;
    }

    void UIDrawList::AddPoints(UIDrawCommand &command, std::span<const UIPoint> points)
    {
        command.mPointOffset = static_cast<uint32_t>(mPoints.size());
        command.mPointCount = static_cast<uint32_t>(points.size());
        mPoints.insert(mPoints.end(), points.begin(), points.end());
    }

    void UIDrawList::Rect(const UIRect &rect, UIColor color, float rounding, const UIGradient &gradient)
    {
        UIDrawCommand command{.mKind = UIDrawCommandKind::Rect, .mRect = rect, .mRounding = rounding};
        if (!rect.IsEmpty() && Begin(command, rect, color, gradient))
            mCommands.push_back(command);
    }

    void UIDrawList::Texture(const UIRect &rect, UITexture texture, UIColor tint, const UIRect &uv)
    {
        UIDrawCommand command{.mKind = UIDrawCommandKind::Texture, .mRect = rect, .mTexture = texture, .mUV = uv};
        if (!rect.IsEmpty() && Begin(command, rect, tint, {}))
            mCommands.push_back(command);
    }

    void UIDrawList::Sprite(const UIRect &rect, const UISprite &sprite, UIColor tint)
    {
        if (!sprite.IsValid() || rect.IsEmpty())
            return;

        const UIRect &source = sprite.mSource;


        const float left = std::clamp(sprite.mSlice.mLeft, 0.0f, source.width);
        const float right = std::clamp(sprite.mSlice.mRight, 0.0f, source.width - left);
        const float top = std::clamp(sprite.mSlice.mTop, 0.0f, source.height);
        const float bottom = std::clamp(sprite.mSlice.mBottom, 0.0f, source.height - top);

        float scale = 1.0f;
        if (left + right > rect.width)
            scale = std::min(scale, rect.width / (left + right));
        if (top + bottom > rect.height)
            scale = std::min(scale, rect.height / (top + bottom));

        const float xs[4] = {rect.x, rect.x + left * scale, rect.Right() - right * scale, rect.Right()};
        const float ys[4] = {rect.y, rect.y + top * scale, rect.Bottom() - bottom * scale, rect.Bottom()};
        const float us[4] = {source.x, source.x + left, source.Right() - right, source.Right()};
        const float vs[4] = {source.y, source.y + top, source.Bottom() - bottom, source.Bottom()};

        for (int row = 0; row < 3; ++row)
        {
            for (int column = 0; column < 3; ++column)
            {
                const UIRect piece{xs[column], ys[row], xs[column + 1] - xs[column], ys[row + 1] - ys[row]};
                if (piece.IsEmpty())
                    continue;

                const UIRect uv{us[column] / sprite.mTextureWidth, vs[row] / sprite.mTextureHeight,
                                (us[column + 1] - us[column]) / sprite.mTextureWidth,
                                (vs[row + 1] - vs[row]) / sprite.mTextureHeight};
                Texture(piece, sprite.mTexture, tint, uv);
            }
        }
    }

    void UIDrawList::Text(float x, float y, std::string_view text, UIColor color, float fontSize, UIFont font)
    {

        const UIRect clip = CurrentClip();
        if (text.empty() || clip.IsEmpty())
            return;

        const uint32_t offset = static_cast<uint32_t>(mTextBuffer.size());
        mTextBuffer.append(text);

        mCommands.push_back(UIDrawCommand{.mKind = UIDrawCommandKind::Text,
                                          .mRect = UIRect{x, y, 0.0f, 0.0f},
                                          .mClip = clip,
                                          .mColor = Faded(color),
                                          .mFontSize = fontSize,
                                          .mFont = font,
                                          .mTextOffset = offset,
                                          .mTextLength = static_cast<uint32_t>(text.size()),
                                          .mTransform = mTransformStack.empty() ? 0 : mTransformStack.back()});
    }

    void UIDrawList::Line(UIPoint from, UIPoint to, UIColor color, const UIStroke &stroke, const UIGradient &gradient)
    {
        const UIPoint points[] = {from, to};
        Polyline(points, color, stroke, false, gradient);
    }

    void UIDrawList::Polyline(std::span<const UIPoint> points, UIColor color, const UIStroke &stroke, bool closed,
                              const UIGradient &gradient)
    {
        if (points.size() < 2 || !(stroke.mWidth > 0.0f))
            return;
        if (stroke.Dashed())
        {
            StrokePath(points, closed, color, stroke, gradient);
            return;
        }

        UIDrawCommand command{.mKind = UIDrawCommandKind::Polyline, .mThickness = stroke.mWidth, .mClosed = closed};
        if (!Begin(command, Inflate(PointBounds(points), stroke.mWidth * 0.5f + 1.0f), color, gradient))
            return;
        AddPoints(command, points);
        mCommands.push_back(command);
    }

    void UIDrawList::StrokePath(std::span<const UIPoint> points, bool closed, UIColor color, const UIStroke &stroke,
                                const UIGradient &gradient)
    {
        const UIStroke solid{.mWidth = stroke.mWidth};
        const std::optional<std::vector<std::vector<UIPoint>>> pieces = Dashes(points, closed, stroke);
        if (!pieces)
        {
            Polyline(points, color, solid, closed, gradient);
            return;
        }
        for (const std::vector<UIPoint> &piece : *pieces)
            Polyline(piece, color, solid, false, gradient);
    }

    void UIDrawList::StrokeRect(const UIRect &rect, UIColor color, const UIStroke &stroke, float rounding,
                                const UIGradient &gradient)
    {
        if (rect.IsEmpty() || !(stroke.mWidth > 0.0f))
            return;
        if (stroke.Dashed())
        {
            StrokePath(RectPath(rect, rounding), true, color, stroke, gradient);
            return;
        }

        UIDrawCommand command{.mKind = UIDrawCommandKind::RectOutline,
                              .mRect = rect,
                              .mRounding = rounding,
                              .mThickness = stroke.mWidth};
        if (Begin(command, Inflate(rect, stroke.mWidth * 0.5f + 1.0f), color, gradient))
            mCommands.push_back(command);
    }

    void UIDrawList::StrokeCircle(UIPoint center, float radius, UIColor color, const UIStroke &stroke,
                                  const UIGradient &gradient)
    {
        if (!(radius > 0.0f) || !(stroke.mWidth > 0.0f))
            return;
        if (stroke.Dashed())
        {
            StrokePath(ArcPoints(center, radius, 0.0f, kTurn, false), true, color, stroke, gradient);
            return;
        }

        UIDrawCommand command{.mKind = UIDrawCommandKind::Circle,
                              .mRect = UIRect{center.x, center.y, radius, radius},
                              .mThickness = stroke.mWidth};
        if (Begin(command, Inflate(CircleBounds(center, radius), stroke.mWidth * 0.5f + 1.0f), color, gradient))
            mCommands.push_back(command);
    }

    void UIDrawList::Arc(UIPoint center, float radius, float from, float to, UIColor color, const UIStroke &stroke,
                         const UIGradient &gradient)
    {
        if (!(radius > 0.0f) || !(stroke.mWidth > 0.0f) || from == to)
            return;
        if (IsFullTurn(from, to))
        {
            StrokeCircle(center, radius, color, stroke, gradient);
            return;
        }
        if (stroke.Dashed())
        {
            StrokePath(ArcPoints(center, radius, from, to, true), false, color, stroke, gradient);
            return;
        }

        UIDrawCommand command{.mKind = UIDrawCommandKind::Arc,
                              .mRect = UIRect{center.x, center.y, radius, radius},
                              .mThickness = stroke.mWidth,
                              .mAngleFrom = from,
                              .mAngleTo = to};
        if (Begin(command, Inflate(CircleBounds(center, radius), stroke.mWidth * 0.5f + 1.0f), color, gradient))
            mCommands.push_back(command);
    }

    void UIDrawList::Polygon(std::span<const UIPoint> points, UIColor color, const UIGradient &gradient)
    {
        if (points.size() < 3)
            return;

        UIDrawCommand command{.mKind = UIDrawCommandKind::Polygon};
        if (!Begin(command, Inflate(PointBounds(points), 1.0f), color, gradient))
            return;
        AddPoints(command, points);
        mCommands.push_back(command);
    }

    void UIDrawList::FillCircle(UIPoint center, float radius, UIColor color, const UIGradient &gradient)
    {
        if (!(radius > 0.0f))
            return;

        UIDrawCommand command{.mKind = UIDrawCommandKind::Circle, .mRect = UIRect{center.x, center.y, radius, radius}};
        if (Begin(command, Inflate(CircleBounds(center, radius), 1.0f), color, gradient))
            mCommands.push_back(command);
    }

    void UIDrawList::Pie(UIPoint center, float radius, float from, float to, UIColor color, const UIGradient &gradient)
    {
        if (!(radius > 0.0f) || from == to)
            return;
        if (IsFullTurn(from, to))
        {
            FillCircle(center, radius, color, gradient);
            return;
        }

        UIDrawCommand command{.mKind = UIDrawCommandKind::Arc,
                              .mRect = UIRect{center.x, center.y, radius, radius},
                              .mAngleFrom = from,
                              .mAngleTo = to};
        if (Begin(command, Inflate(CircleBounds(center, radius), 1.0f), color, gradient))
            mCommands.push_back(command);
    }

    void UIDrawList::PushClip(const UIRect &rect)
    {
        // an empty rect's transformed bounds can have area, so it stays empty
        const UIRect clip = CurrentClip();
        mClipStack.push_back(rect.IsEmpty() ? UIRect{clip.x, clip.y, 0.0f, 0.0f}
                                            : Intersect(clip, CurrentTransform().Bounds(rect)));
    }

    void UIDrawList::PopClip()
    {
        MIR_ASSERT(mClipStack.size() > 1, "UIDrawList::PopClip without a matching PushClip");

        if (mClipStack.size() > 1)
            mClipStack.pop_back();
    }

    UIRect UIDrawList::CurrentClip() const { return mClipStack.empty() ? UIRect{} : mClipStack.back(); }

    void UIDrawList::PushOpacity(float opacity)
    {
        // NaN reads as fully faded rather than reaching the backend
        const float clamped = opacity > 0.0f ? std::min(opacity, 1.0f) : 0.0f;
        mOpacityStack.push_back(CurrentOpacity() * clamped);
    }

    void UIDrawList::PopOpacity()
    {
        MIR_ASSERT(mOpacityStack.size() > 1, "UIDrawList::PopOpacity without a matching PushOpacity");

        if (mOpacityStack.size() > 1)
            mOpacityStack.pop_back();
    }

    void UIDrawList::PushTransform(const UITransform &transform)
    {
        if (mTransformStack.empty())
        {
            mTransforms.assign(1, UITransform{});
            mTransformStack.assign(1, 0);
        }
        const UITransform combined = CurrentTransform() * transform;
        mTransforms.push_back(combined);
        mTransformStack.push_back(static_cast<uint32_t>(mTransforms.size() - 1));
    }

    void UIDrawList::PopTransform()
    {
        MIR_ASSERT(mTransformStack.size() > 1, "UIDrawList::PopTransform without a matching PushTransform");

        if (mTransformStack.size() > 1)
            mTransformStack.pop_back();
    }

    const UITransform &UIDrawList::CurrentTransform() const
    {
        static const UITransform kIdentity{};
        return mTransformStack.empty() ? kIdentity : mTransforms[mTransformStack.back()];
    }

    UIColor UIDrawList::Faded(UIColor color) const
    {
        color.a *= CurrentOpacity();
        return color;
    }

    std::string_view UIDrawList::TextOf(const UIDrawCommand &command) const
    {
        if (command.mKind != UIDrawCommandKind::Text)
            return {};

        return std::string_view(mTextBuffer).substr(command.mTextOffset, command.mTextLength);
    }

    std::span<const UIPoint> UIDrawList::PointsOf(const UIDrawCommand &command) const
    {
        if (command.mKind != UIDrawCommandKind::Polyline && command.mKind != UIDrawCommandKind::Polygon)
            return {};

        return std::span<const UIPoint>(mPoints).subspan(command.mPointOffset, command.mPointCount);
    }

    const UITransform &UIDrawList::TransformOf(const UIDrawCommand &command) const
    {
        static const UITransform kIdentity{};
        return command.mTransform < mTransforms.size() ? mTransforms[command.mTransform] : kIdentity;
    }

    const UIGradient *UIDrawList::GradientOf(const UIDrawCommand &command) const
    {
        if (command.mGradient == 0 || command.mGradient > mGradients.size())
            return nullptr;
        return &mGradients[command.mGradient - 1];
    }
} // namespace mir
