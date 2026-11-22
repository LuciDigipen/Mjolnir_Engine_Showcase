/**
 * @file UITypes.h
 * @author Quy Dong Ta
 * @brief Value types shared by the game UI: a rect, a point, a color, a
 *        sprite, a font and a style.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <algorithm>
#include <cstdint>

namespace mir
{
    // pos goes top left origin, y down
    struct UIRect
    {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;

        constexpr float Right() const { return x + width; }
        constexpr float Bottom() const { return y + height; }
        constexpr bool IsEmpty() const { return width <= 0.0f || height <= 0.0f; }
        constexpr bool operator==(const UIRect &) const = default;
    };

    constexpr UIRect Intersect(const UIRect &a, const UIRect &b)
    {
        const float left = std::max(a.x, b.x);
        const float top = std::max(a.y, b.y);
        const float right = std::min(a.Right(), b.Right());
        const float bottom = std::min(a.Bottom(), b.Bottom());
        return UIRect{left, top, std::max(right - left, 0.0f), std::max(bottom - top, 0.0f)};
    }

    constexpr bool Contains(const UIRect &rect, float x, float y)
    {
        return x >= rect.x && x < rect.Right() && y >= rect.y && y < rect.Bottom();
    }

    struct UISize
    {
        float width = 0.0f;
        float height = 0.0f;

        constexpr bool operator==(const UISize &) const = default;
    };

    struct UIPoint
    {
        float x = 0.0f;
        float y = 0.0f;

        constexpr bool operator==(const UIPoint &) const = default;
    };
    struct UIColor
    {
        float r = 1.0f;
        float g = 1.0f;
        float b = 1.0f;
        float a = 1.0f;

        constexpr bool operator==(const UIColor &) const = default;
    };

    inline constexpr UIColor kUIWhite{1.0f, 1.0f, 1.0f, 1.0f};

    //texture held by UITextures, 0 is no texture
    struct UITexture
    {
        uint32_t mId = 0;

        constexpr bool IsValid() const { return mId != 0; }
        constexpr bool operator==(const UITexture &) const = default;
    };

    // a font held by UIFonts, 0 is the backend's default font
    struct UIFont
    {
        uint32_t mId = 0;

        constexpr bool IsValid() const { return mId != 0; }
        constexpr bool operator==(const UIFont &) const = default;
    };

    enum class UITextAlign : uint8_t
    {
        Left,
        Center,
        Right,
    };

    struct UIEdges
    {
        float mLeft = 0.0f;
        float mTop = 0.0f;
        float mRight = 0.0f;
        float mBottom = 0.0f;

        constexpr bool operator==(const UIEdges &) const = default;
    };


    struct UISprite
    {
        UITexture mTexture;


        float mTextureWidth = 0.0f;
        float mTextureHeight = 0.0f;


        UIRect mSource;
        UIEdges mSlice;

        constexpr bool IsValid() const
        {
            return mTexture.IsValid() && mTextureWidth > 0.0f && mTextureHeight > 0.0f && !mSource.IsEmpty();
        }
        constexpr bool operator==(const UISprite &) const = default;
    };

    struct UIStyle
    {
        UIColor mTextColor = kUIWhite;
        UIColor mPanelColor{0.0f, 0.0f, 0.0f, 0.6f};
        float mPanelRounding = 4.0f;

        // in UI units, so text scales with the boxes it sits in. 0 is the backend's own size, which in the editor
        // follows the monitor's DPI and not the UI's scale, so it can outgrow its boxes
        float mFontSize = 16.0f;


        float mTextHeight = 16.0f;

        UIFont mFont;
        // where a widget's text sits across it, inside the padding
        UITextAlign mTextAlign = UITextAlign::Left;

        // drawn under the text while their alpha is above 0, and faded with it
        UIColor mTextShadowColor{0.0f, 0.0f, 0.0f, 0.0f};
        float mTextShadowX = 1.0f;
        float mTextShadowY = 1.0f;
        UIColor mTextOutlineColor{0.0f, 0.0f, 0.0f, 0.0f};
        float mTextOutlineWidth = 1.0f;

        UIColor mWidgetColor{0.25f, 0.25f, 0.25f, 0.9f};
        UIColor mWidgetHoveredColor{0.35f, 0.35f, 0.35f, 0.9f};
        UIColor mWidgetHeldColor{0.15f, 0.15f, 0.15f, 0.95f};
        UIColor mDisabledTextColor{0.55f, 0.55f, 0.55f, 1.0f};
        UIColor mTrackColor{0.1f, 0.1f, 0.1f, 0.9f};
        UIColor mFillColor{0.3f, 0.6f, 0.9f, 1.0f};
        UIColor mFocusColor{1.0f, 0.8f, 0.2f, 1.0f};
        float mWidgetRounding = 3.0f;
        float mFocusThickness = 2.0f;
        float mPadding = 8.0f;

        UISprite mPanelSprite;
        UISprite mWidgetSprite;
        UISprite mWidgetHoveredSprite;
        UISprite mWidgetHeldSprite;
        UISprite mTrackSprite;
        UISprite mFillSprite;
        UISprite mFocusSprite;
    };
} // namespace mir
