/**
 * @file UIDrawList.h
 * @author Quy Dong Ta
 * @brief Records rects, shapes, textures, nine-sliced sprites and text under
 *        clip, opacity and transform stacks, in coordinates local to the UI
 *        area, for the backend to replay at the end of a frame.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITransform.h"
#include "UITypes.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    enum class UIDrawCommandKind : uint8_t
    {
        Rect,
        Texture,
        Text,
        RectOutline,
        // mPoints as a line, round when mClosed
        Polyline,
        // mPoints filled, convex or not
        Polygon,
        // centre mRect.x, mRect.y and radius mRect.width, filled when mThickness is 0
        Circle,
        // a circle's mAngleFrom to mAngleTo, in radians, a pie slice when mThickness is 0
        Arc,
    };

    // a line's width, and dashes of mDash with mGap between them when both are above 0. mDashOffset moves them
    // along the line
    struct UIStroke
    {
        float mWidth = 1.0f;
        float mDash = 0.0f;
        float mGap = 0.0f;
        float mDashOffset = 0.0f;

        bool Dashed() const { return mDash > 0.0f && mGap > 0.0f; }
    };

    // multiplies a shape's colour, from mFromColor at mFrom to mToColor at mTo and flat past either end. Points
    // are where the shape is drawn, before any transform
    struct UIGradient
    {
        UIPoint mFrom;
        UIPoint mTo;
        UIColor mFromColor;
        UIColor mToColor;

        bool IsSet() const { return mFrom != mTo; }
        bool operator==(const UIGradient &) const = default;
    };

    struct UIDrawCommand
    {
        UIDrawCommandKind mKind = UIDrawCommandKind::Rect;

        UIRect mRect;

        // in the UI area's own units, already through the transform
        UIRect mClip;

        UIColor mColor;

        float mRounding = 0.0f;

        float mFontSize = 0.0f;
        UIFont mFont;

        UITexture mTexture;
        UIRect mUV{0.0f, 0.0f, 1.0f, 1.0f};

        uint32_t mTextOffset = 0;
        uint32_t mTextLength = 0;

        float mThickness = 0.0f;
        float mAngleFrom = 0.0f;
        float mAngleTo = 0.0f;
        bool mClosed = false;
        uint32_t mPointOffset = 0;
        uint32_t mPointCount = 0;

        // into the list's transforms and gradients, read with TransformOf and GradientOf
        uint32_t mTransform = 0;
        uint32_t mGradient = 0;
    };

    class UIDrawList
    {
    public:

        void Reset(float width, float height);

        void Rect(const UIRect &rect, UIColor color, float rounding = 0.0f, const UIGradient &gradient = {});
        void Texture(const UIRect &rect, UITexture texture, UIColor tint = kUIWhite,
                     const UIRect &uv = UIRect{0.0f, 0.0f, 1.0f, 1.0f});

        void Sprite(const UIRect &rect, const UISprite &sprite, UIColor tint = kUIWhite);
        void Text(float x, float y, std::string_view text, UIColor color, float fontSize = 0.0f, UIFont font = {});

        // A dashed stroke is recorded as one open Polyline a dash
        void Line(UIPoint from, UIPoint to, UIColor color, const UIStroke &stroke = {},
                  const UIGradient &gradient = {});
        void Polyline(std::span<const UIPoint> points, UIColor color, const UIStroke &stroke = {}, bool closed = false,
                      const UIGradient &gradient = {});
        void StrokeRect(const UIRect &rect, UIColor color, const UIStroke &stroke = {}, float rounding = 0.0f,
                        const UIGradient &gradient = {});
        void StrokeCircle(UIPoint center, float radius, UIColor color, const UIStroke &stroke = {},
                          const UIGradient &gradient = {});
        // from and to in radians, 0 to the right and clockwise; a full turn or more is a circle
        void Arc(UIPoint center, float radius, float from, float to, UIColor color, const UIStroke &stroke = {},
                 const UIGradient &gradient = {});

        // Fills
        void Polygon(std::span<const UIPoint> points, UIColor color, const UIGradient &gradient = {});
        void FillCircle(UIPoint center, float radius, UIColor color, const UIGradient &gradient = {});
        void Pie(UIPoint center, float radius, float from, float to, UIColor color, const UIGradient &gradient = {});

        // the clip is kept in the area's units: a rect pushed under a transform clips to its transformed bounds
        void PushClip(const UIRect &rect);
        void PopClip();

        UIRect CurrentClip() const;

        size_t ClipDepth() const { return mClipStack.empty() ? 0 : mClipStack.size() - 1; }

        // multiplies the alpha of everything drawn until the matching pop, nested pushes multiply.
        // Clamped to [0, 1]. A faded widget still takes the pointer
        void PushOpacity(float opacity);
        void PopOpacity();

        float CurrentOpacity() const { return mOpacityStack.empty() ? 1.0f : mOpacityStack.back(); }

        size_t OpacityDepth() const { return mOpacityStack.empty() ? 0 : mOpacityStack.size() - 1; }

        // moves, turns and scales everything drawn until the matching pop, nested pushes inside the outer ones
        void PushTransform(const UITransform &transform);
        void PopTransform();

        const UITransform &CurrentTransform() const;

        size_t TransformDepth() const { return mTransformStack.empty() ? 0 : mTransformStack.size() - 1; }

        std::span<const UIDrawCommand> Commands() const { return mCommands; }
        std::string_view TextOf(const UIDrawCommand &command) const;
        std::span<const UIPoint> PointsOf(const UIDrawCommand &command) const;
        const UITransform &TransformOf(const UIDrawCommand &command) const;
        // null when the command has none
        const UIGradient *GradientOf(const UIDrawCommand &command) const;

    private:
        UIColor Faded(UIColor color) const;

        // a command with the current clip, transform and colour, or false when its local bounds are clipped away
        bool Begin(UIDrawCommand &command, const UIRect &bounds, UIColor color, const UIGradient &gradient);
        void AddPoints(UIDrawCommand &command, std::span<const UIPoint> points);
        void StrokePath(std::span<const UIPoint> points, bool closed, UIColor color, const UIStroke &stroke,
                        const UIGradient &gradient);

        std::vector<UIDrawCommand> mCommands;

        std::vector<UIRect> mClipStack;

        std::vector<float> mOpacityStack;

        // [0] is the identity, the stack holds indices into it
        std::vector<UITransform> mTransforms;
        std::vector<uint32_t> mTransformStack;

        // [index - 1]
        std::vector<UIGradient> mGradients;

        std::vector<UIPoint> mPoints;

        std::string mTextBuffer;
    };
} // namespace mir
