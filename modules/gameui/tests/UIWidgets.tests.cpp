/**
 * @file UIWidgets.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIWidgets and the demo screen: each widget's result
 *        by pointer and by UI action, the slider by drag and by step, the
 *        display widgets staying inert, what each draws, and the style alone
 *        swapping grey box for art. No backend: what is drawn is read from
 *        the draw list.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDemoScreen.h>
#include <gameui/UIWidgets.h>

#include <input/DefaultInputMap.h>
#include <input/InputState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    constexpr UIRect kArea{0, 0, 800, 600};

    constexpr UIRect kRect{100, 100, 200, 40};
    constexpr UIPointer kRest{700, 550, false};
    constexpr UIPointer kOver{150, 120, false};
    constexpr UIPointer kPressOver{150, 120, true};

    // the UI actions for one frame, as InputSystem would leave them
    struct Actions
    {
        float mNavigateX = 0.0f;
        float mNavigateY = 0.0f;
        bool mConfirm = false;
    };

    constexpr Actions Nav(float x, float y) { return Actions{.mNavigateX = x, .mNavigateY = y}; }
    constexpr Actions kConfirm{.mConfirm = true};

    class Fixture
    {
    public:
        Fixture() { mInput.SetActiveDevice(DeviceKind::Gamepad); }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        // one frame of game UI
        void Frame(const std::function<void(UIContext &)> &draw, const UIPointer &pointer = kRest,
                   const Actions &actions = {})
        {
            SetButton(kUIConfirmAction, actions.mConfirm);
            ActionValue &navigate = mInput.Actions()[std::string(kUINavigateAction)];
            const bool down = actions.mNavigateX != 0.0f || actions.mNavigateY != 0.0f;
            navigate.justPressed = down && !navigate.down;
            navigate.down = down;
            navigate.x = actions.mNavigateX;
            navigate.y = actions.mNavigateY;

            mUI.BeginFrame(mArea, mInput, mMap, pointer);
            draw(mUI);
            mUI.EndFrame();
        }

        UIContext &UI() { return mUI; }
        InputState &Input() { return mInput; }

        // the area the next frames draw into
        void SetArea(const UIRect &area) { mArea = area; }

    private:
        void SetButton(std::string_view action, bool down)
        {
            ActionValue &value = mInput.Actions()[std::string(action)];
            value.justPressed = down && !value.down;
            value.down = down;
        }

        UIRect mArea = kArea;
        UIContext mUI;
        InputState mInput;
        InputMap mMap = DefaultInputMap();
    };

    size_t Count(const UIDrawList &list, UIDrawCommandKind kind)
    {
        size_t count = 0;
        for (const UIDrawCommand &command : list.Commands())
            count += command.mKind == kind ? 1 : 0;
        return count;
    }

    size_t CountRects(const UIDrawList &list) { return Count(list, UIDrawCommandKind::Rect); }

    // a sprite with an 8 texel border round a 24x24 texture; ids need not be
    // loaded, since an unknown texture draws as a flat rect
    constexpr UISprite Frame(uint32_t texture)
    {
        return UISprite{.mTexture = UITexture{texture},
                        .mTextureWidth = 24.0f,
                        .mTextureHeight = 24.0f,
                        .mSource = UIRect{0, 0, 24, 24},
                        .mSlice = UIEdges{8, 8, 8, 8}};
    }

    // the one texture every Texture command in the list draws; null if none
    // or several
    UITexture OnlyTexture(const UIDrawList &list)
    {
        UITexture found;
        for (const UIDrawCommand &command : list.Commands())
        {
            if (command.mKind != UIDrawCommandKind::Texture)
                continue;
            if (found.IsValid() && command.mTexture != found)
                return UITexture{};
            found = command.mTexture;
        }
        return found;
    }
} // namespace

TEST_CASE("Button clicks on a press and release over it, and on UIConfirm while focused", "[gameui]")
{
    Fixture f;
    constexpr UIId kId("button");
    UIWidgetResult result;
    const auto draw = [&](UIContext &ui) { result = Button(ui, kId, kRect, "Play"); };

    f.Frame(draw, kOver);
    f.Frame(draw, kOver);
    CHECK(result.mHovered);

    f.Frame(draw, kPressOver);
    f.Frame(draw, kPressOver);
    CHECK(result.mHeld);
    CHECK_FALSE(result.mClicked);

    f.Frame(draw, kOver);
    CHECK(result.mClicked);
    CHECK(f.UI().State().Focus() == kId);

    f.Frame(draw, kRest);
    f.Frame(draw, kRest);
    CHECK_FALSE(result.mClicked);
    f.Frame(draw, kRest, kConfirm);
    CHECK(result.mClicked);
}

TEST_CASE("A disabled Button neither clicks nor takes focus", "[gameui]")
{
    Fixture f;
    UIWidgetResult result;
    const auto draw = [&](UIContext &ui) { result = Button(ui, UIId("button"), kRect, "Play", {.mDisabled = true}); };

    f.Frame(draw, kOver);
    f.Frame(draw, kPressOver);
    f.Frame(draw, kOver);
    CHECK_FALSE(result.mClicked);
    CHECK(f.UI().State().Focus().IsNull());
}

TEST_CASE("Toggle flips its value once per click and once per UIConfirm", "[gameui]")
{
    Fixture f;
    constexpr UIId kId("toggle");
    bool value = false;
    UIWidgetResult result;
    const auto draw = [&](UIContext &ui) { result = Toggle(ui, kId, kRect, "Sound", value); };

    f.Frame(draw, kOver);
    f.Frame(draw, kPressOver);
    CHECK_FALSE(value);
    f.Frame(draw, kOver);
    CHECK(value);
    CHECK(result.mChanged);
    f.Frame(draw, kOver);
    CHECK(value);
    CHECK_FALSE(result.mChanged);

    f.Frame(draw, kRest);
    f.Frame(draw, kRest, kConfirm);
    CHECK_FALSE(value);
    CHECK(result.mChanged);
    f.Frame(draw, kRest, kConfirm);
    CHECK_FALSE(value);
}

TEST_CASE("Slider follows a drag across its rect, snaps to its step and clamps past its ends", "[gameui]")
{
    Fixture f;
    float value = 0.0f;
    UIWidgetResult result;
    const auto draw = [&](UIContext &ui) { result = Slider(ui, UIId("slider"), kRect, value, 0.0f, 10.0f, 1.0f); };

    f.Frame(draw, UIPointer{150, 120, false});
    f.Frame(draw, UIPointer{150, 120, true});
    f.Frame(draw, UIPointer{150, 120, true});
    CHECK(value == Catch::Approx(3.0f)); // 50 of 200 across is 2.5, snapped
    CHECK(result.mChanged);

    // latched: the drag keeps going outside the rect
    f.Frame(draw, UIPointer{600, 400, true});
    CHECK(value == Catch::Approx(10.0f));
    f.Frame(draw, UIPointer{0, 0, true});
    CHECK(value == Catch::Approx(0.0f));

    f.Frame(draw, UIPointer{0, 0, false});
    f.Frame(draw, UIPointer{250, 120, false});
    CHECK(value == Catch::Approx(0.0f));
    CHECK_FALSE(result.mChanged);
}

TEST_CASE("A moved slider takes its value from where it is drawn", "[gameui][transform]")
{
    Fixture f;
    float value = 0.0f;
    const auto draw = [&](UIContext &ui) {
        ui.DrawList().PushTransform(UITransform::Translate(300, 0));
        Slider(ui, UIId("slider"), kRect, value, 0.0f, 100.0f);
        ui.DrawList().PopTransform();
    };

    // the drawn slider spans 400 to 600
    f.Frame(draw, UIPointer{500, 120, false});
    f.Frame(draw, UIPointer{500, 120, true});
    f.Frame(draw, UIPointer{500, 120, true});
    CHECK(value == Catch::Approx(50.0f));
    f.Frame(draw, UIPointer{450, 120, true});
    CHECK(value == Catch::Approx(25.0f));
}

TEST_CASE("Slider steps left and right while focused, without moving focus", "[gameui]")
{
    Fixture f;
    constexpr UIId kSlider("slider");
    constexpr UIId kOther("other");
    float value = 0.5f;
    UIWidgetResult result;
    const auto draw = [&](UIContext &ui)
    {
        result = Slider(ui, kSlider, kRect, value, 0.0f, 1.0f, 0.25f, {.mLinks = {kOther, kOther, kOther, kOther}});
        Button(ui, kOther, UIRect{100, 200, 200, 40}, "Other", {.mLinks = {kSlider, kSlider, kSlider, kSlider}});
    };
    f.UI().State().SetFocus(kSlider);

    f.Frame(draw, kRest, Nav(1, 0));
    CHECK(value == Catch::Approx(0.75f));
    CHECK(result.mChanged);
    CHECK(f.UI().State().Focus() == kSlider);

    f.Frame(draw, kRest, Nav(1, 0)); // held: once per press
    CHECK(value == Catch::Approx(0.75f));

    f.Frame(draw);
    f.Frame(draw, kRest, Nav(1, 0));
    f.Frame(draw);
    f.Frame(draw, kRest, Nav(1, 0));
    CHECK(value == Catch::Approx(1.0f));
    CHECK_FALSE(result.mChanged);

    f.Frame(draw);
    f.Frame(draw, kRest, Nav(-1, 0));
    CHECK(value == Catch::Approx(0.75f));

    f.Frame(draw);
    f.Frame(draw, kRest, Nav(0, 1));
    CHECK(f.UI().State().Focus() == kOther);
}

TEST_CASE("Slider with step 0 moves a twentieth of its range per step", "[gameui]")
{
    Fixture f;
    constexpr UIId kId("slider");
    float value = 0.0f;
    const auto draw = [&](UIContext &ui) { Slider(ui, kId, kRect, value, 0.0f, 2.0f); };
    f.UI().State().SetFocus(kId);

    f.Frame(draw, kRest, Nav(1, 0));
    CHECK(value == Catch::Approx(0.1f));
}

TEST_CASE("A disabled Slider ignores a drag and a step", "[gameui]")
{
    Fixture f;
    constexpr UIId kId("slider");
    float value = 0.5f;
    const auto draw = [&](UIContext &ui) { Slider(ui, kId, kRect, value, 0.0f, 1.0f, 0.1f, {.mDisabled = true}); };
    f.UI().State().SetFocus(kId);

    f.Frame(draw, kRest, Nav(1, 0));
    f.Frame(draw, kOver);
    f.Frame(draw, kPressOver);
    f.Frame(draw, UIPointer{290, 120, true});
    CHECK(value == Catch::Approx(0.5f));
}

TEST_CASE("Label, Image and Bar are hovered but never take focus or click, unless focusable", "[gameui]")
{
    Fixture f;
    constexpr UIId kLabel("label");
    constexpr UIId kImage("image");
    constexpr UIId kBar("bar");
    constexpr UIRect kImageRect{100, 200, 40, 40};
    constexpr UIRect kBarRect{100, 300, 200, 16};
    UIWidgetResult label;
    UIWidgetResult image;
    UIWidgetResult bar;
    bool focusable = false;
    const auto draw = [&](UIContext &ui)
    {
        label = Label(ui, kLabel, kRect, "Hello", {.mFocusable = focusable});
        image = Image(ui, kImage, kImageRect, UISprite{}, kUIWhite, {.mFocusable = focusable});
        bar = Bar(ui, kBar, kBarRect, 0.5f, {.mFocusable = focusable});
    };

    f.Frame(draw, kOver);
    f.Frame(draw, UIPointer{151, 120, true});
    f.Frame(draw, UIPointer{151, 120, false});
    CHECK(label.mHovered);
    CHECK_FALSE(label.mClicked);
    CHECK(f.UI().State().Focus().IsNull());

    f.Frame(draw, UIPointer{110, 210, false});
    f.Frame(draw, UIPointer{111, 210, false});
    CHECK(image.mHovered);
    f.Frame(draw, UIPointer{110, 305, false});
    f.Frame(draw, UIPointer{111, 305, false});
    CHECK(bar.mHovered);
    CHECK(f.UI().State().Focus().IsNull());

    focusable = true;
    f.Frame(draw, kOver);
    f.Frame(draw, kRest, kConfirm);
    CHECK(f.UI().State().Focus() == kLabel);
    CHECK(label.mClicked);
}

TEST_CASE("Bar fills its clamped fraction of the track", "[gameui]")
{
    Fixture f;
    float fraction = 0.25f;
    UIRect fill;
    size_t rects = 0;
    const auto draw = [&](UIContext &ui)
    {
        Bar(ui, UIId("bar"), kRect, fraction);
        rects = CountRects(ui.DrawList());
        fill = ui.DrawList().Commands().back().mRect;
    };

    f.Frame(draw);
    CHECK(rects == 2);
    CHECK(fill.width == Catch::Approx(50.0f));

    fraction = 3.0f;
    f.Frame(draw);
    CHECK(fill.width == Catch::Approx(kRect.width));

    fraction = -1.0f;
    f.Frame(draw);
    CHECK(rects == 1);
}

TEST_CASE("A widget swaps from grey box to art through its style alone", "[gameui]")
{
    Fixture f;
    constexpr UIId kId("button");
    size_t rects = 0;
    size_t textures = 0;
    UITexture texture;
    // the one call site, for both looks
    const auto draw = [&](UIContext &ui)
    {
        Button(ui, kId, kRect, "Play");
        rects = CountRects(ui.DrawList());
        textures = Count(ui.DrawList(), UIDrawCommandKind::Texture);
        texture = OnlyTexture(ui.DrawList());
    };
    // the press below focuses the button; no ring under the mouse keeps the counts to the face
    f.Input().SetActiveDevice(DeviceKind::Mouse);

    f.Frame(draw);
    CHECK(rects == 1);
    CHECK(textures == 0);

    f.UI().Style().mWidgetSprite = Frame(1);
    f.Frame(draw);
    CHECK(rects == 0);
    CHECK(textures == 9);
    CHECK(texture == UITexture{1});

    // a state with its own sprite draws it; one without keeps the face's art
    f.UI().Style().mWidgetHoveredSprite = Frame(2);
    f.Frame(draw, kOver);
    f.Frame(draw, kOver);
    CHECK(texture == UITexture{2});
    f.Frame(draw, kPressOver);
    f.Frame(draw, kPressOver);
    CHECK(texture == UITexture{1});

    f.UI().Style().mWidgetSprite = UISprite{};
    f.UI().Style().mWidgetHoveredSprite = UISprite{};
    f.Frame(draw);
    CHECK(rects == 1);
    CHECK(textures == 0);
}

TEST_CASE("Every grey-box part has a sprite in the style", "[gameui]")
{
    Fixture f;
    UIStyle &style = f.UI().Style();
    style.mPanelSprite = Frame(1);
    style.mWidgetSprite = Frame(2);
    style.mTrackSprite = Frame(3);
    style.mFillSprite = Frame(4);
    style.mFocusSprite = Frame(5);
    f.UI().State().SetFocus(UIId("slider"));
    bool on = true;
    float value = 0.5f;
    size_t rects = 0;
    std::vector<UITexture> drawn;
    const auto draw = [&](UIContext &ui)
    {
        Panel(ui, UIRect{0, 0, 400, 400});
        Toggle(ui, UIId("toggle"), UIRect{100, 50, 200, 40}, "Toggle", on);
        Slider(ui, UIId("slider"), kRect, value, 0.0f, 1.0f);
        Bar(ui, UIId("bar"), UIRect{100, 200, 200, 16}, 0.5f);
        rects = CountRects(ui.DrawList());
        drawn.clear();
        for (const UIDrawCommand &command : ui.DrawList().Commands())
            if (command.mKind == UIDrawCommandKind::Texture &&
                (drawn.empty() || drawn.back() != command.mTexture))
                drawn.push_back(command.mTexture);
    };

    f.Frame(draw);
    CHECK(rects == 0);
    // panel; toggle face and its box filled; focus ring, slider track, fill
    // and knob; bar track and fill
    const std::vector<UITexture> expected{UITexture{1}, UITexture{2}, UITexture{4}, UITexture{5}, UITexture{3},
                                          UITexture{4}, UITexture{2}, UITexture{3}, UITexture{4}};
    CHECK(drawn == expected);
}

TEST_CASE("Image draws its sprite tinted, and a rect of the tint without one", "[gameui]")
{
    Fixture f;
    constexpr UIColor kTint{0.5f, 1.0f, 0.5f, 1.0f};
    UISprite sprite;
    const UIDrawCommand *first = nullptr;
    size_t count = 0;
    const auto draw = [&](UIContext &ui)
    {
        Image(ui, UIId("image"), kRect, sprite, kTint);
        count = ui.DrawList().Commands().size();
        first = count > 0 ? &ui.DrawList().Commands()[0] : nullptr;
    };

    f.Frame(draw);
    REQUIRE(count == 1);
    CHECK(first->mKind == UIDrawCommandKind::Rect);
    CHECK(first->mColor == kTint);

    sprite = Frame(7);
    sprite.mSlice = UIEdges{};
    f.Frame(draw);
    REQUIRE(count == 1);
    CHECK(first->mKind == UIDrawCommandKind::Texture);
    CHECK(first->mTexture == UITexture{7});
    CHECK(first->mColor == kTint);
    CHECK(first->mRect == kRect);
}

TEST_CASE("The focus ring is drawn only while the focus highlight is shown", "[gameui]")
{
    Fixture f;
    constexpr UIId kId("button");
    size_t rects = 0;
    const auto draw = [&](UIContext &ui)
    {
        Button(ui, kId, kRect, "Play");
        rects = CountRects(ui.DrawList());
    };
    f.UI().State().SetFocus(kId);

    f.Frame(draw);
    CHECK(rects == 2);

    f.Input().SetActiveDevice(DeviceKind::Mouse);
    f.Frame(draw);
    CHECK(rects == 1);
}

TEST_CASE("A hidden widget draws nothing and is still declared for its links", "[gameui]")
{
    Fixture f;
    constexpr UIId kTop("top");
    constexpr UIId kHidden("hidden");
    constexpr UIId kBottom("bottom");
    bool value = false;
    const auto draw = [&](UIContext &ui)
    {
        Button(ui, kTop, kRect, "Top", {.mLinks = {UIId{}, kHidden, UIId{}, UIId{}}});
        const size_t before = ui.DrawList().Commands().size();
        Toggle(ui, kHidden, UIRect{100, 150, 200, 40}, "Hidden", value,
               {.mLinks = {kTop, kBottom, UIId{}, UIId{}}, .mHidden = true});
        CHECK(ui.DrawList().Commands().size() == before);
        Button(ui, kBottom, UIRect{100, 200, 200, 40}, "Bottom", {.mLinks = {kHidden, UIId{}, UIId{}, UIId{}}});
    };
    f.UI().State().SetFocus(kTop);

    f.Frame(draw, kRest, Nav(0, 1));
    CHECK(f.UI().State().Focus() == kBottom);
}

TEST_CASE("The demo screen walks its column by gamepad and drives the bar from the slider", "[gameui]")
{
    Fixture f;
    UIDemoScreen demo;
    const auto draw = [&](UIContext &ui) { demo.Draw(ui); };

    f.Frame(draw);
    CHECK(f.UI().State().Focus() == UIId("demo_button"));

    f.Frame(draw, kRest, kConfirm);
    CHECK(demo.Clicks() == 1);

    f.Frame(draw, kRest, Nav(0, 1));
    f.Frame(draw);
    CHECK(f.UI().State().Focus() == UIId("demo_toggle"));
    f.Frame(draw, kRest, kConfirm);
    CHECK_FALSE(demo.Toggled());

    // down passes over the disabled button to the slider
    f.Frame(draw, kRest, Nav(0, 1));
    f.Frame(draw);
    CHECK(f.UI().State().Focus() == UIId("demo_slider"));

    f.Frame(draw, kRest, Nav(1, 0));
    CHECK(demo.Volume() == Catch::Approx(0.55f));
    CHECK(f.UI().State().Focus() == UIId("demo_slider"));
}

TEST_CASE("The demo screen draws inside the context's safe area at 16:9, 21:9 and 4:3", "[gameui][layout]")
{
    Fixture f;
    UIDemoScreen demo;
    UIRect panel;
    const auto draw = [&](UIContext &ui)
    {
        demo.Draw(ui);
        panel = UIDemoScreen::Layout(ui.SafeArea()).mPanel;
    };

    for (const UIRect area : {UIRect{0, 0, 1920, 1080}, UIRect{0, 0, 2560, 1080}, UIRect{0, 0, 1440, 1080}})
    {
        f.SetArea(area);
        for (const UIEdges insets : {UIEdges{}, UIEdges{area.width * 0.05f, area.height * 0.05f,
                                                         area.width * 0.05f, area.height * 0.05f}})
        {
            f.UI().SetSafeAreaInsets(insets);
            f.Frame(draw);

            // every rect, texture and line of text the screen drew
            const UIRect safe = f.UI().SafeArea();
            // the area in UI units, which a 1080-line display draws half as large again
            CHECK(safe == Inset(f.UI().Bounds(), insets));
            CHECK(f.UI().Bounds().height == Catch::Approx(area.height / f.UI().Scale()));
            REQUIRE_FALSE(f.UI().DrawList().Commands().empty());
            for (const UIDrawCommand &command : f.UI().DrawList().Commands())
            {
                CHECK(command.mRect.x >= safe.x);
                CHECK(command.mRect.y >= safe.y);
                CHECK(command.mRect.Right() <= safe.Right());
                CHECK(command.mRect.Bottom() <= safe.Bottom());
            }

            // the inset moves the menu, not a constant in the screen
            CHECK(panel.x == Catch::Approx(insets.mLeft + 12.0f));
        }
    }
}

TEST_CASE("An area below the design size scales the whole UI down: bounds and the pointer are in UI units",
          "[gameui][layout]")
{
    Fixture f;
    UIWidgetResult result;
    const auto draw = [&](UIContext &ui)
    {
        result = Button(ui, UIId("button"), kRect, "Play");
    };

    // half the design size, offset in the window as the editor's viewport is
    constexpr UIRect kHalf{300, 80, 320, 240};
    f.SetArea(kHalf);

    // kRect's middle, (200, 120) in UI units, in window pixels
    const UIPointer over{kHalf.x + 200.0f * 0.5f, kHalf.y + 120.0f * 0.5f, false};
    f.Frame(draw, over);
    f.Frame(draw, over);
    CHECK(f.UI().Scale() == Catch::Approx(0.5f));
    CHECK(f.UI().Bounds() == UIRect{0, 0, 640, 480});
    CHECK(f.UI().DrawList().CurrentClip() == UIRect{0, 0, 640, 480});
    CHECK(result.mHovered);

    SECTION("the narrower side sets the scale")
    {
        f.SetArea(UIRect{0, 0, 160, 480});
        f.Frame(draw);
        CHECK(f.UI().Scale() == Catch::Approx(0.25f));
        CHECK(f.UI().Bounds().width == Catch::Approx(640.0f));
    }
    SECTION("with no reference height a larger area is never scaled up")
    {
        f.UI().SetReferenceHeight(0.0f);
        f.SetArea(UIRect{0, 0, 1920, 1080});
        f.Frame(draw);
        CHECK(f.UI().Scale() == 1.0f);
        CHECK(f.UI().Bounds() == UIRect{0, 0, 1920, 1080});
    }
    SECTION("a design size of 0 leaves the area as it is")
    {
        f.UI().SetDesignSize(0.0f, 0.0f);
        f.Frame(draw);
        CHECK(f.UI().Scale() == 1.0f);
        CHECK(f.UI().Bounds() == UIRect{0, 0, 320, 240});
    }
}

TEST_CASE("An area taller than the reference height scales the UI up with it, so it takes the same share of any "
          "display",
          "[gameui][layout]")
{
    Fixture f;
    const auto nothing = [](UIContext &) {};
    const auto scaleAt = [&](const UIRect &area)
    {
        f.SetArea(area);
        f.Frame(nothing);
        return f.UI().Scale();
    };

    REQUIRE(f.UI().ReferenceHeight() == 720.0f);

    // at or under the reference height, one pixel a unit
    CHECK(scaleAt(UIRect{0, 0, 1280, 720}) == 1.0f);
    CHECK(scaleAt(UIRect{0, 0, 1000, 600}) == 1.0f);

    // above it, by height, whatever the aspect ratio
    CHECK(scaleAt(UIRect{0, 0, 1920, 1080}) == Catch::Approx(1.5f));
    CHECK(scaleAt(UIRect{0, 0, 2560, 1080}) == Catch::Approx(1.5f));
    CHECK(scaleAt(UIRect{0, 0, 3840, 2160}) == Catch::Approx(3.0f));
    CHECK(f.UI().Bounds() == UIRect{0, 0, 1280, 720});

    // never so far that the design size stops fitting across a narrow area
    CHECK(scaleAt(UIRect{0, 0, 800, 2000}) == Catch::Approx(800.0f / 640.0f));
    CHECK(f.UI().Bounds().width == Catch::Approx(640.0f));

    // and an area below the design size still shrinks to fit it
    CHECK(scaleAt(UIRect{0, 0, 320, 240}) == Catch::Approx(0.5f));
}

namespace
{
    // measures in fixed pixels a character, and keeps the size it was asked for
    class MeasuringBackend final : public IUIBackend
    {
    public:
        UIBackendTexture CreateTexture(uint32_t, uint32_t, std::span<const uint8_t>) override { return {}; }
        UIBackendFont MakeFont(std::span<const uint8_t>) override
        {
            return UIBackendFont{ reinterpret_cast<void *>(static_cast<uintptr_t>(++mMadeFonts)), [](void *) {} };
        }
        void Submit(const UIRect &, float, const UIDrawList &, const UITextures &, const UIFonts &) override {}
        UISize MeasureText(std::string_view text, float fontSize, void *font) const override
        {
            mAskedSize = fontSize;
            mAskedFont = font;
            return UISize{10.0f * static_cast<float>(text.size()), fontSize};
        }

        mutable float mAskedSize = -1.0f;
        mutable void *mAskedFont = nullptr;
        int mMadeFonts = 0;
    };
}

TEST_CASE("Text is measured in UI units at the style's font size, by the backend when there is one",
          "[gameui][text]")
{
    Fixture f;

    SECTION("without a backend, an estimate of half the font size a character")
    {
        f.Frame([](UIContext &ui) { CHECK(ui.MeasureText("Pause") == UISize{40.0f, 16.0f}); });
    }

    SECTION("by the backend, at the size it is drawn, given back in UI units")
    {
        MeasuringBackend backend;
        f.UI().SetBackend(&backend);
        f.SetArea(UIRect{0, 0, 1920, 1080});
        f.Frame(
            [&](UIContext &ui)
            {
                REQUIRE(ui.Scale() == Catch::Approx(1.5f));
                const UISize size = ui.MeasureText("Pause");
                CHECK(backend.mAskedSize == Catch::Approx(16.0f * 1.5f));
                CHECK(size.width == Catch::Approx(50.0f / 1.5f));
                CHECK(size.height == Catch::Approx(16.0f));
            });
        f.UI().SetBackend(nullptr);
    }
}

namespace
{
    std::vector<UIDrawCommand> Texts(const UIDrawList &list)
    {
        std::vector<UIDrawCommand> texts;
        for (const UIDrawCommand &command : list.Commands())
            if (command.mKind == UIDrawCommandKind::Text)
                texts.push_back(command);
        return texts;
    }
}

TEST_CASE("A widget's text is drawn in the style's font and size, over its shadow and outline", "[gameui][text]")
{
    Fixture f;
    UIStyle &style = f.UI().Style();
    style.mFont = UIFont{7};
    style.mFontSize = 20.0f;
    const auto label = [&](UIColor color = kUIWhite)
    {
        style.mTextColor = color;
        f.Frame([](UIContext &ui) { Label(ui, UIId("label"), kRect, "Speed"); });
        return Texts(f.UI().DrawList());
    };

    SECTION("plain, one text in the font")
    {
        const std::vector<UIDrawCommand> texts = label();
        REQUIRE(texts.size() == 1);
        CHECK(texts[0].mFont == UIFont{7});
        CHECK(texts[0].mFontSize == 20.0f);
    }

    SECTION("a shadow first, offset and in its own colour")
    {
        style.mTextShadowColor = UIColor{0.0f, 0.0f, 0.0f, 0.5f};
        style.mTextShadowX = 2.0f;
        style.mTextShadowY = 3.0f;
        const std::vector<UIDrawCommand> texts = label();
        REQUIRE(texts.size() == 2);
        CHECK(texts[0].mRect.x == Catch::Approx(texts[1].mRect.x + 2.0f));
        CHECK(texts[0].mRect.y == Catch::Approx(texts[1].mRect.y + 3.0f));
        CHECK(texts[0].mColor == UIColor{0.0f, 0.0f, 0.0f, 0.5f});
        CHECK(texts[1].mColor == kUIWhite);
        CHECK(texts[0].mFont == UIFont{7});
    }

    SECTION("an outline of eight copies round it, as far out as its width, faded with the text")
    {
        style.mTextShadowColor = UIColor{0.0f, 0.0f, 0.0f, 0.5f};
        style.mTextOutlineColor = UIColor{0.0f, 0.0f, 0.0f, 1.0f};
        style.mTextOutlineWidth = 2.0f;
        const std::vector<UIDrawCommand> texts = label(UIColor{1.0f, 1.0f, 1.0f, 0.5f});
        REQUIRE(texts.size() == 10);
        const UIDrawCommand &text = texts.back();
        CHECK(texts[0].mColor.a == Catch::Approx(0.25f));
        for (size_t i = 1; i < 9; ++i)
        {
            const float dx = texts[i].mRect.x - text.mRect.x;
            const float dy = texts[i].mRect.y - text.mRect.y;
            CHECK(std::sqrt(dx * dx + dy * dy) == Catch::Approx(2.0f));
            CHECK(texts[i].mColor.a == Catch::Approx(0.5f));
        }
    }

    SECTION("a clear shadow or outline, or one 0 wide, is not drawn")
    {
        style.mTextShadowColor = UIColor{0.0f, 0.0f, 0.0f, 0.0f};
        style.mTextOutlineColor = UIColor{0.0f, 0.0f, 0.0f, 1.0f};
        style.mTextOutlineWidth = 0.0f;
        CHECK(label().size() == 1);
    }
}

TEST_CASE("A widget's text sits left, centred or right inside its padding", "[gameui][text]")
{
    Fixture f;
    UIStyle &style = f.UI().Style();
    // without a backend "Pause" is estimated 40 wide at 16
    const auto textX = [&](UITextAlign align)
    {
        style.mTextAlign = align;
        f.Frame([](UIContext &ui) { Label(ui, UIId("label"), kRect, "Pause"); });
        const std::vector<UIDrawCommand> texts = Texts(f.UI().DrawList());
        REQUIRE(texts.size() == 1);
        return texts[0].mRect.x;
    };

    const float padding = style.mPadding;
    CHECK(textX(UITextAlign::Left) == Catch::Approx(kRect.x + padding));
    CHECK(textX(UITextAlign::Right) == Catch::Approx(kRect.Right() - padding - 40.0f));
    CHECK(textX(UITextAlign::Center) == Catch::Approx(kRect.x + (kRect.width - 40.0f) * 0.5f));
}

TEST_CASE("Text is measured in the font it is drawn in", "[gameui][text]")
{
    Fixture f;
    MeasuringBackend backend;
    // a TrueType tag and one 16 byte table at 28, which the backend here never reads
    const std::vector<uint8_t> header = [] {
        std::vector<uint8_t> bytes(200, 0);
        bytes[1] = 1;
        bytes[5] = 1;
        bytes[12 + 11] = 28;
        bytes[12 + 15] = 16;
        return bytes;
    }();
    const UIFont font = f.UI().Fonts().Add(header);
    REQUIRE(font.IsValid());
    f.UI().SetBackend(&backend);
    REQUIRE(backend.mMadeFonts == 1);

    f.Frame(
        [&](UIContext &ui)
        {
            ui.Style().mFont = font;
            ui.MeasureText("Speed");
            CHECK(backend.mAskedFont == ui.Fonts().Find(font));
            CHECK(backend.mAskedFont != nullptr);

            const UISize size = ui.MeasureText("Speed", UIFont{}, 10.0f);
            CHECK(backend.mAskedFont == nullptr);
            CHECK(backend.mAskedSize == Catch::Approx(10.0f));
            CHECK(size.height == Catch::Approx(10.0f));
        });
    f.UI().SetBackend(nullptr);
}
