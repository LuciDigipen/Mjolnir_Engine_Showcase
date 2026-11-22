/**
 * @file UIStyleFile.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for reading a UI style file: fields by name over the base
 *        style, sprites and fonts through the caller's loaders, and each
 *        wrong field reported and left as the base had it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameshell/UIStyleFile.h>

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    constexpr UISprite kPanel{.mTexture = UITexture{7}, .mTextureWidth = 64, .mTextureHeight = 64,
                              .mSource = UIRect{0, 0, 64, 64}};
    constexpr UIFont kFont{3};

    // panel.png and title.ttf load, anything else does not
    struct Loads
    {
        std::vector<std::string> mAsked;

        UIStyleAssets Assets()
        {
            return UIStyleAssets{
                .mSprite = [this](std::string_view path)
                {
                    mAsked.emplace_back(path);
                    return path == "ui/panel.png" ? kPanel : UISprite{};
                },
                .mFont = [this](std::string_view path)
                {
                    mAsked.emplace_back(path);
                    return path == "fonts/title.ttf" ? kFont : UIFont{};
                },
            };
        }
    };

    bool Mentions(const std::vector<std::string> &errors, std::string_view text)
    {
        for (const std::string &error : errors)
            if (error.find(text) != std::string::npos)
                return true;
        return false;
    }
} // namespace

TEST_CASE("A style file sets the fields it names over the base style", "[gameshell][style]")
{
    Loads loads;
    UIStyle base;
    base.mPadding = 4.0f;
    std::vector<std::string> errors;
    const UIStyle style = ParseUIStyle(R"({
        "widget_color": [0.5, 0.1, 0.1],
        "focus_color": [1, 1, 0, 0.5],
        "widget_rounding": 6,
        "text_shadow_x": -2,
        "text_align": "center",
        "font": "fonts/title.ttf",
        "panel_sprite": { "path": "ui/panel.png", "slice": [1, 2, 3, 4] },
        "track_sprite": "ui/panel.png"
    })", base, loads.Assets(), errors);

    CHECK(errors.empty());
    CHECK(style.mWidgetColor == UIColor{0.5f, 0.1f, 0.1f, 1.0f});
    CHECK(style.mFocusColor == UIColor{1, 1, 0, 0.5f});
    CHECK(style.mWidgetRounding == 6.0f);
    CHECK(style.mTextShadowX == -2.0f);
    CHECK(style.mTextAlign == UITextAlign::Center);
    CHECK(style.mFont == kFont);
    CHECK(style.mPanelSprite.mTexture == kPanel.mTexture);
    CHECK(style.mPanelSprite.mSlice == UIEdges{1, 2, 3, 4});
    CHECK(style.mTrackSprite.mTexture == kPanel.mTexture);
    CHECK(style.mTrackSprite.mSlice == UIEdges{});

    // what the file leaves out is the base's
    CHECK(style.mPadding == 4.0f);
    CHECK(style.mTextColor == base.mTextColor);
    CHECK(loads.mAsked == std::vector<std::string>{"fonts/title.ttf", "ui/panel.png", "ui/panel.png"});
}

TEST_CASE("A slice of one number cuts every edge, and null clears a sprite or the font", "[gameshell][style]")
{
    Loads loads;
    UIStyle base;
    base.mWidgetSprite = kPanel;
    base.mFont = kFont;
    std::vector<std::string> errors;
    const UIStyle style = ParseUIStyle(
        R"({ "fill_sprite": { "path": "ui/panel.png", "slice": 8 }, "widget_sprite": null, "font": null })", base,
        loads.Assets(), errors);
    CHECK(errors.empty());
    CHECK(style.mFillSprite.mSlice == UIEdges{8, 8, 8, 8});
    CHECK_FALSE(style.mWidgetSprite.IsValid());
    CHECK_FALSE(style.mFont.IsValid());
}

TEST_CASE("Each wrong field is reported and keeps the base's value, the rest still apply", "[gameshell][style]")
{
    Loads loads;
    const UIStyle base;
    std::vector<std::string> errors;
    const UIStyle style = ParseUIStyle(R"({
        "text_colour": [1, 0, 0],
        "panel_color": [1, 0],
        "padding": -1,
        "widget_rounding": "round",
        "text_align": "middle",
        "font": "fonts/missing.ttf",
        "focus_sprite": { "path": "ui/panel.png", "slice": [1, 2] },
        "fill_sprite": "ui/missing.png",
        "track_sprite": { "file": "ui/panel.png" },
        "focus_thickness": 5
    })", base, loads.Assets(), errors);

    CHECK(errors.size() == 9);
    CHECK(Mentions(errors, "unknown key 'text_colour'; valid ones are text_color, panel_color"));
    CHECK(Mentions(errors, "panel_color must be [r, g, b] or [r, g, b, a]"));
    CHECK(Mentions(errors, "padding must be 0 or more"));
    CHECK(Mentions(errors, "widget_rounding must be a number"));
    CHECK(Mentions(errors, "text_align must be \"left\", \"center\" or \"right\""));
    CHECK(Mentions(errors, "font 'fonts/missing.ttf' did not load as a font"));
    CHECK(Mentions(errors, "focus_sprite slice must be a number or [left, top, right, bottom]"));
    CHECK(Mentions(errors, "fill_sprite 'ui/missing.png' did not load as a texture"));
    CHECK(Mentions(errors, "track_sprite has an unknown key 'file'"));

    CHECK(style.mPanelColor == base.mPanelColor);
    CHECK(style.mPadding == base.mPadding);
    CHECK(style.mTextAlign == base.mTextAlign);
    CHECK_FALSE(style.mFocusSprite.IsValid());
    CHECK(style.mFocusThickness == 5.0f);
}

TEST_CASE("A style file that is not a JSON object is the base style, with one error", "[gameshell][style]")
{
    Loads loads;
    UIStyle base;
    base.mPadding = 2.0f;
    for (const std::string_view text : {"{ \"padding\": ", "[1, 2]", ""})
    {
        std::vector<std::string> errors;
        const UIStyle style = ParseUIStyle(text, base, loads.Assets(), errors);
        CHECK(errors.size() == 1);
        CHECK(style.mPadding == 2.0f);
    }
}
