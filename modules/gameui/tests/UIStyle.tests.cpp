/**
 * @file UIStyle.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the style stack on UIContext and the table of style
 *        fields: pushes nest over the base style, a widget draws in the top
 *        one, and every field is found by its name.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>
#include <gameui/UIStyleFields.h>
#include <gameui/UIWidgets.h>

#include <input/InputMap.h>
#include <input/InputState.h>

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <string_view>
#include <variant>

using namespace mir;

namespace
{
    UIStyle Padded(float padding)
    {
        UIStyle style;
        style.mPadding = padding;
        return style;
    }
} // namespace

TEST_CASE("PushStyle and PopStyle nest over the base style", "[gameui][style]")
{
    UIContext ui;
    CHECK(ui.StyleDepth() == 0);
    CHECK(ui.Style().mPadding == UIStyle{}.mPadding);

    ui.SetBaseStyle(Padded(5));
    CHECK(ui.Style().mPadding == 5);

    ui.PushStyle(Padded(2));
    ui.PushStyle(Padded(3));
    CHECK(ui.StyleDepth() == 2);
    CHECK(ui.Style().mPadding == 3);
    ui.PopStyle();
    CHECK(ui.Style().mPadding == 2);
    ui.PopStyle();
    CHECK(ui.StyleDepth() == 0);
    CHECK(ui.Style().mPadding == 5);
    CHECK(ui.BaseStyle().mPadding == 5);
}

TEST_CASE("A widget draws in the style on top of the stack, and the base style once it is popped", "[gameui][style]")
{
    InputState input;
    InputMap map;
    UIContext ui;
    constexpr UIColor kRed{1, 0, 0, 1};
    constexpr UIRect kRect{10, 10, 100, 30};

    const auto faceColor = [&](bool styled)
    {
        ui.BeginFrame(UIRect{0, 0, 640, 480}, input, map, UIPointer{600, 400, false});
        UIStyle red = ui.Style();
        red.mWidgetColor = kRed;
        if (styled)
            ui.PushStyle(red);
        Button(ui, UIId("button"), kRect, "Buy");
        if (styled)
            ui.PopStyle();
        const UIColor face = ui.DrawList().Commands()[0].mColor;
        ui.EndFrame();
        return face;
    };
    CHECK(faceColor(true) == kRed);
    CHECK(faceColor(false) == UIStyle{}.mWidgetColor);
}

TEST_CASE("Every style field has its own name, and the name finds it", "[gameui][style]")
{
    std::set<std::string_view> names;
    for (const UIStyleField &field : UIStyleFields())
    {
        CHECK(names.insert(field.mName).second);
        CHECK(FindUIStyleField(field.mName) == &field);
        CHECK(std::string(UIStyleFieldNames()).find(field.mName) != std::string::npos);
    }
    CHECK(names.size() == 29);
    CHECK(FindUIStyleField("text_colour") == nullptr);

    // a field writes its own member
    UIStyle style;
    const UIStyleField *fill = FindUIStyleField("fill_color");
    REQUIRE(fill != nullptr);
    style.*std::get<UIColor UIStyle::*>(fill->mMember) = UIColor{0, 0, 1, 1};
    CHECK(style.mFillColor == UIColor{0, 0, 1, 1});
    CHECK(style.mWidgetColor == UIStyle{}.mWidgetColor);

    // only a shadow's offset may go below 0
    for (const UIStyleField &field : UIStyleFields())
        CHECK(field.mSigned == (field.mName == "text_shadow_x" || field.mName == "text_shadow_y"));
}

TEST_CASE("Text alignment reads and writes by name", "[gameui][style]")
{
    for (const UITextAlign align : {UITextAlign::Left, UITextAlign::Center, UITextAlign::Right})
        CHECK(ParseUITextAlign(UITextAlignName(align)) == align);
    CHECK_FALSE(ParseUITextAlign("middle"));
}
