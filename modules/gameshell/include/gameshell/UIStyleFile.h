/**
 * @file UIStyleFile.h
 * @author Quy Dong Ta
 * @brief A game's UI style file: a JSON object of UIStyle fields by name, read
 *        over a base style, with its sprites and font loaded by the caller.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <gameui/UITypes.h>

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // by asset path, an invalid sprite or font is reported as one that did not load
    struct UIStyleAssets
    {
        std::function<UISprite(std::string_view path)> mSprite;
        std::function<UIFont(std::string_view path)> mFont;
    };

    // A field missing from the file keeps base's value, and so does one that is wrong, with why in errors. A file
    // that is not a JSON object is base, with one error
    UIStyle ParseUIStyle(std::string_view text, const UIStyle &base, const UIStyleAssets &assets,
                         std::vector<std::string> &errors);
} // namespace mir
