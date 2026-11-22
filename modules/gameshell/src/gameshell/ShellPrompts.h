/**
 * @file ShellPrompts.h
 * @author Quy Dong Ta
 * @brief Button prompts for the shell's screens: a key cap or pad pill with
 *        the binding's label, then what it does.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UIContext.h>
#include <gameui/UILayout.h>
#include <gameui/UIWidgets.h>

#include <algorithm>
#include <string_view>

namespace mir::shell
{
    inline constexpr float kCapPadding = 6.0f;
    inline constexpr float kCaptionGap = 6.0f;

    // as wide as its label and the padding round it, and never narrower than tall
    inline float CapWidth(const UIContext &ui, std::string_view label, float height)
    {
        return std::max(height, ui.TextWidth(label) + 2.0f * kCapPadding);
    }

    // a line of text centred down @p rect from @p x
    inline void DrawLine(UIContext &ui, const UIRect &rect, float x, std::string_view text, UIColor color)
    {
        const float height = ui.MeasureText(text).height;
        StyledText(ui, x, rect.y + (rect.height - height) * 0.5f, text, color);
    }

    // the label centred in the cap both ways
    inline void DrawCap(UIContext &ui, const UIRect &rect, const UIPrompt &prompt)
    {
        ui.DrawList().Rect(rect, UIColor{0.9f, 0.9f, 0.9f, 0.9f}, prompt.mGamepad ? rect.height * 0.5f : 3.0f);
        const float x = rect.x + (rect.width - ui.TextWidth(prompt.mLabel)) * 0.5f;
        DrawLine(ui, rect, x, prompt.mLabel, UIColor{0.0f, 0.0f, 0.0f, 1.0f});
    }

    inline void DrawPrompt(UIContext &ui, UIStack &strip, std::string_view action, std::string_view text)
    {
        const UIPrompt prompt = ui.Prompt(action);
        if (prompt.mLabel.empty())
            return;

        const UIRect bounds = strip.Bounds();
        const float capWidth = CapWidth(ui, prompt.mLabel, bounds.height);
        UIStack parts = UIStack::Row(strip.Last(capWidth + kCaptionGap + ui.TextWidth(text)), kCaptionGap);
        DrawCap(ui, parts.Next(capWidth), prompt);

        const UIRect caption = parts.Rest();
        DrawLine(ui, caption, caption.x, text, ui.Style().mTextColor);
    }
} // namespace mir::shell
