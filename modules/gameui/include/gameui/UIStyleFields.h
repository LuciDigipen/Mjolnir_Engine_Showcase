/**
 * @file UIStyleFields.h
 * @author Quy Dong Ta
 * @brief Every UIStyle field by its snake_case name and kind, the one list a
 *        style file and a script read, so neither keeps its own.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UITypes.h"

#include <optional>
#include <span>
#include <string_view>
#include <variant>

namespace mir
{
    using UIStyleMember = std::variant<UIColor UIStyle::*, float UIStyle::*, UITextAlign UIStyle::*,
                                       UIFont UIStyle::*, UISprite UIStyle::*>;

    struct UIStyleField
    {
        std::string_view mName;
        UIStyleMember mMember;
        // a number below 0 is refused unless signed, as a shadow's offset is
        bool mSigned = false;
    };

    std::span<const UIStyleField> UIStyleFields();

    const UIStyleField *FindUIStyleField(std::string_view name);

    // "text_color, panel_color, ...", for an error naming an unknown key
    std::string_view UIStyleFieldNames();

    std::optional<UITextAlign> ParseUITextAlign(std::string_view name);
    std::string_view UITextAlignName(UITextAlign align);
} // namespace mir
