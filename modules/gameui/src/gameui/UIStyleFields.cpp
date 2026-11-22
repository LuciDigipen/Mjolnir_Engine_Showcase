/**
 * @file UIStyleFields.cpp
 * @author Quy Dong Ta
 * @brief Every UIStyle field by its snake_case name and kind, the one list a
 *        style file and a script read, so neither keeps its own.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIStyleFields.h>

#include <string>

namespace mir
{
    namespace
    {
        // in UIStyle's order
        const UIStyleField kFields[] = {
            {"text_color", &UIStyle::mTextColor},
            {"panel_color", &UIStyle::mPanelColor},
            {"panel_rounding", &UIStyle::mPanelRounding},
            {"font_size", &UIStyle::mFontSize},
            {"text_height", &UIStyle::mTextHeight},
            {"font", &UIStyle::mFont},
            {"text_align", &UIStyle::mTextAlign},
            {"text_shadow_color", &UIStyle::mTextShadowColor},
            {"text_shadow_x", &UIStyle::mTextShadowX, true},
            {"text_shadow_y", &UIStyle::mTextShadowY, true},
            {"text_outline_color", &UIStyle::mTextOutlineColor},
            {"text_outline_width", &UIStyle::mTextOutlineWidth},
            {"widget_color", &UIStyle::mWidgetColor},
            {"widget_hovered_color", &UIStyle::mWidgetHoveredColor},
            {"widget_held_color", &UIStyle::mWidgetHeldColor},
            {"disabled_text_color", &UIStyle::mDisabledTextColor},
            {"track_color", &UIStyle::mTrackColor},
            {"fill_color", &UIStyle::mFillColor},
            {"focus_color", &UIStyle::mFocusColor},
            {"widget_rounding", &UIStyle::mWidgetRounding},
            {"focus_thickness", &UIStyle::mFocusThickness},
            {"padding", &UIStyle::mPadding},
            {"panel_sprite", &UIStyle::mPanelSprite},
            {"widget_sprite", &UIStyle::mWidgetSprite},
            {"widget_hovered_sprite", &UIStyle::mWidgetHoveredSprite},
            {"widget_held_sprite", &UIStyle::mWidgetHeldSprite},
            {"track_sprite", &UIStyle::mTrackSprite},
            {"fill_sprite", &UIStyle::mFillSprite},
            {"focus_sprite", &UIStyle::mFocusSprite},
        };
    } // namespace

    std::span<const UIStyleField> UIStyleFields() { return kFields; }

    const UIStyleField *FindUIStyleField(std::string_view name)
    {
        for (const UIStyleField &field : kFields)
            if (field.mName == name)
                return &field;
        return nullptr;
    }

    std::string_view UIStyleFieldNames()
    {
        static const std::string names = []
        {
            std::string joined;
            for (const UIStyleField &field : kFields)
            {
                if (!joined.empty())
                    joined += ", ";
                joined += field.mName;
            }
            return joined;
        }();
        return names;
    }

    std::optional<UITextAlign> ParseUITextAlign(std::string_view name)
    {
        if (name == "left")
            return UITextAlign::Left;
        if (name == "center")
            return UITextAlign::Center;
        if (name == "right")
            return UITextAlign::Right;
        return std::nullopt;
    }

    std::string_view UITextAlignName(UITextAlign align)
    {
        switch (align)
        {
        case UITextAlign::Center:
            return "center";
        case UITextAlign::Right:
            return "right";
        case UITextAlign::Left:
            break;
        }
        return "left";
    }
} // namespace mir
