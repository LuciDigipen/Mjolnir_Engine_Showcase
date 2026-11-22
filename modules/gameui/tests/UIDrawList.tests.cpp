/**
 * @file UIDrawList.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameui/UIDrawList: recording, the clip stack, culling,
 *        nine-slice sprites, shapes, dashes, gradients and the transform stack.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIDrawList.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <numbers>
#include <vector>

using namespace mir;

namespace
{
    // a 24x24 texture with an 8 texel border all round
    constexpr UISprite kFrame{.mTexture = UITexture{1},
                              .mTextureWidth = 24.0f,
                              .mTextureHeight = 24.0f,
                              .mSource = UIRect{0, 0, 24, 24},
                              .mSlice = UIEdges{8, 8, 8, 8}};

    std::vector<UIDrawCommand> SpriteCommands(const UIRect &rect, const UISprite &sprite, float areaSize = 1000.0f)
    {
        UIDrawList list;
        list.Reset(areaSize, areaSize);
        list.Sprite(rect, sprite);
        return {list.Commands().begin(), list.Commands().end()};
    }

    constexpr float kTurn = 2.0f * std::numbers::pi_v<float>;

    void CheckPoint(UIPoint actual, UIPoint expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x).margin(1e-4));
        CHECK(actual.y == Catch::Approx(expected.y).margin(1e-4));
    }

    // each Polyline's points, in order
    std::vector<std::vector<UIPoint>> Lines(const UIDrawList &list)
    {
        std::vector<std::vector<UIPoint>> lines;
        for (const UIDrawCommand &command : list.Commands())
            if (command.mKind == UIDrawCommandKind::Polyline)
                lines.emplace_back(list.PointsOf(command).begin(), list.PointsOf(command).end());
        return lines;
    }

    void CheckRect(const UIRect &actual, const UIRect &expected)
    {
        CHECK(actual.x == Catch::Approx(expected.x));
        CHECK(actual.y == Catch::Approx(expected.y));
        CHECK(actual.width == Catch::Approx(expected.width));
        CHECK(actual.height == Catch::Approx(expected.height));
    }
} // namespace

TEST_CASE("Intersect of disjoint rects is empty, not negative", "[gameui]")
{
    const UIRect overlap = Intersect(UIRect{0, 0, 10, 10}, UIRect{20, 20, 5, 5});

    CHECK(overlap.IsEmpty());
    CHECK(overlap.width == 0.0f);
    CHECK(overlap.height == 0.0f);
}

TEST_CASE("Reset sets the root clip to the area and empties the list", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 50);
    list.Rect(UIRect{0, 0, 10, 10}, kUIWhite);
    list.Reset(200, 80);

    CHECK(list.Commands().empty());
    CHECK(list.ClipDepth() == 0);
    CHECK(list.CurrentClip() == UIRect{0, 0, 200, 80});
}

TEST_CASE("Commands record in order with their kind and the current clip", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    const UITexture texture{3};
    list.Rect(UIRect{10, 10, 20, 20}, UIColor{1, 0, 0, 1}, 4.0f);
    list.Texture(UIRect{40, 40, 10, 10}, texture, kUIWhite, UIRect{0.5f, 0, 0.5f, 1});
    list.Text(5, 6, "hello", kUIWhite, 18.0f);

    const auto commands = list.Commands();
    REQUIRE(commands.size() == 3);

    CHECK(commands[0].mKind == UIDrawCommandKind::Rect);
    CHECK(commands[0].mRect == UIRect{10, 10, 20, 20});
    CHECK(commands[0].mRounding == 4.0f);
    CHECK(commands[0].mClip == UIRect{0, 0, 100, 100});

    CHECK(commands[1].mKind == UIDrawCommandKind::Texture);
    CHECK(commands[1].mTexture == texture);
    CHECK(commands[1].mUV == UIRect{0.5f, 0, 0.5f, 1});

    CHECK(commands[2].mKind == UIDrawCommandKind::Text);
    CHECK(commands[2].mRect.x == 5.0f);
    CHECK(commands[2].mRect.y == 6.0f);
    CHECK(commands[2].mFontSize == 18.0f);
    CHECK(list.TextOf(commands[2]) == "hello");
}

TEST_CASE("Text keeps each string separate in the shared buffer", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);
    list.Text(0, 0, "first", kUIWhite);
    list.Text(0, 20, "second", kUIWhite);

    REQUIRE(list.Commands().size() == 2);
    CHECK(list.TextOf(list.Commands()[0]) == "first");
    CHECK(list.TextOf(list.Commands()[1]) == "second");
}

TEST_CASE("PushClip intersects with the current clip and PopClip restores it", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    list.PushClip(UIRect{50, 50, 100, 100});
    CHECK(list.ClipDepth() == 1);
    CHECK(list.CurrentClip() == UIRect{50, 50, 50, 50});

    list.PushClip(UIRect{0, 0, 60, 60});
    CHECK(list.CurrentClip() == UIRect{50, 50, 10, 10});

    list.Rect(UIRect{0, 0, 100, 100}, kUIWhite);
    REQUIRE(list.Commands().size() == 1);
    CHECK(list.Commands()[0].mClip == UIRect{50, 50, 10, 10});

    list.PopClip();
    list.PopClip();
    CHECK(list.ClipDepth() == 0);
    CHECK(list.CurrentClip() == UIRect{0, 0, 100, 100});
}

TEST_CASE("An empty clip stays empty under a turn", "[gameui][transform]")
{
    UIDrawList list;
    list.Reset(100, 100);

    list.PushTransform(UITransform::Rotate(kTurn / 8.0f, {50, 50}));
    list.PushClip(UIRect{50, 30, 0, 40});
    CHECK(list.CurrentClip().IsEmpty());
    list.Rect(UIRect{40, 40, 20, 20}, kUIWhite);
    list.PopClip();
    list.PopTransform();
    CHECK(list.Commands().empty());
}

TEST_CASE("Draws entirely outside the clip are not recorded", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    list.Rect(UIRect{200, 200, 10, 10}, kUIWhite);
    list.Texture(UIRect{-50, 0, 10, 10}, UITexture{});
    CHECK(list.Commands().empty());

    // text is not measured, so only an empty clip culls it
    list.Text(500, 500, "offscreen", kUIWhite);
    CHECK(list.Commands().size() == 1);

    list.PushClip(UIRect{200, 200, 10, 10});
    list.Text(0, 0, "clipped away", kUIWhite);
    list.Rect(UIRect{0, 0, 100, 100}, kUIWhite);
    list.PopClip();
    CHECK(list.Commands().size() == 1);
}

TEST_CASE("TextOf a non-text command is empty", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);
    list.Rect(UIRect{0, 0, 10, 10}, kUIWhite);

    REQUIRE(list.Commands().size() == 1);
    CHECK(list.TextOf(list.Commands()[0]).empty());
}

TEST_CASE("A sprite with no slice stretches its source over the rect as one texture", "[gameui]")
{
    // the second of four 16x16 cells in a 64x32 atlas
    const UISprite cell{.mTexture = UITexture{2},
                        .mTextureWidth = 64.0f,
                        .mTextureHeight = 32.0f,
                        .mSource = UIRect{16, 0, 16, 16}};

    UIDrawList list;
    list.Reset(500, 500);
    list.Sprite(UIRect{10, 20, 100, 50}, cell, UIColor{1, 0, 0, 1});

    REQUIRE(list.Commands().size() == 1);
    const UIDrawCommand &command = list.Commands()[0];
    CHECK(command.mKind == UIDrawCommandKind::Texture);
    CHECK(command.mTexture == UITexture{2});
    CHECK(command.mRect == UIRect{10, 20, 100, 50});
    CHECK(command.mColor == UIColor{1, 0, 0, 1});
    CheckRect(command.mUV, UIRect{0.25f, 0.0f, 0.25f, 0.5f});
}

TEST_CASE("Nine-slice keeps the corners at their texel size and stretches the edges and middle", "[gameui]")
{
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{10, 20, 200, 60}, kFrame);
    REQUIRE(commands.size() == 9);

    // row by row, left to right
    CheckRect(commands[0].mRect, UIRect{10, 20, 8, 8});
    CheckRect(commands[1].mRect, UIRect{18, 20, 184, 8});
    CheckRect(commands[2].mRect, UIRect{202, 20, 8, 8});
    CheckRect(commands[3].mRect, UIRect{10, 28, 8, 44});
    CheckRect(commands[4].mRect, UIRect{18, 28, 184, 44});
    CheckRect(commands[5].mRect, UIRect{202, 28, 8, 44});
    CheckRect(commands[6].mRect, UIRect{10, 72, 8, 8});
    CheckRect(commands[7].mRect, UIRect{18, 72, 184, 8});
    CheckRect(commands[8].mRect, UIRect{202, 72, 8, 8});

    // each slice samples its own third of the texture
    constexpr float kThird = 1.0f / 3.0f;
    CheckRect(commands[0].mUV, UIRect{0, 0, kThird, kThird});
    CheckRect(commands[4].mUV, UIRect{kThird, kThird, kThird, kThird});
    CheckRect(commands[8].mUV, UIRect{2 * kThird, 2 * kThird, kThird, kThird});
    for (const UIDrawCommand &command : commands)
        CHECK(command.mTexture == kFrame.mTexture);
}

TEST_CASE("Nine-slice corners stay the same at any size large enough to hold them", "[gameui]")
{
    for (const float width : {16.0f, 17.0f, 48.0f, 333.0f, 1000.0f})
    {
        for (const float height : {16.0f, 30.0f, 250.0f})
        {
            const UIRect rect{5, 7, width, height};
            const std::vector<UIDrawCommand> commands = SpriteCommands(rect, kFrame, 2000.0f);

            // a middle of zero width or height has no piece to draw
            const size_t columns = width > 16.0f ? 3 : 2;
            const size_t rows = height > 16.0f ? 3 : 2;
            REQUIRE(commands.size() == columns * rows);

            CheckRect(commands.front().mRect, UIRect{rect.x, rect.y, 8, 8});
            CheckRect(commands.back().mRect, UIRect{rect.Right() - 8, rect.Bottom() - 8, 8, 8});

            // the pieces cover the rect exactly
            float area = 0.0f;
            for (const UIDrawCommand &command : commands)
                area += command.mRect.width * command.mRect.height;
            CHECK(area == Catch::Approx(width * height));
        }
    }
}

TEST_CASE("Nine-slice shrinks all four corners by one factor in a rect smaller than two of them", "[gameui]")
{
    // 12 across holds 0.75 of two 8 texel corners; 40 down would hold them whole
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{0, 0, 12, 40}, kFrame);

    // no middle column, so two pieces per row
    REQUIRE(commands.size() == 6);
    CheckRect(commands[0].mRect, UIRect{0, 0, 6, 6});
    CheckRect(commands[1].mRect, UIRect{6, 0, 6, 6});
    CheckRect(commands[2].mRect, UIRect{0, 6, 6, 28});
    CheckRect(commands[3].mRect, UIRect{6, 6, 6, 28});
    CheckRect(commands[4].mRect, UIRect{0, 34, 6, 6});
    CheckRect(commands[5].mRect, UIRect{6, 34, 6, 6});

    // the corners still sample the whole of their texels
    CheckRect(commands[0].mUV, UIRect{0, 0, 1.0f / 3.0f, 1.0f / 3.0f});
}

TEST_CASE("Nine-slice honours a different slice on each side", "[gameui]")
{
    UISprite sprite = kFrame;
    sprite.mSlice = UIEdges{4, 2, 12, 6};
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{0, 0, 100, 50}, sprite);

    REQUIRE(commands.size() == 9);
    CheckRect(commands[0].mRect, UIRect{0, 0, 4, 2});
    CheckRect(commands[2].mRect, UIRect{88, 0, 12, 2});
    CheckRect(commands[6].mRect, UIRect{0, 44, 4, 6});
    CheckRect(commands[8].mRect, UIRect{88, 44, 12, 6});
    CheckRect(commands[4].mUV, UIRect{4.0f / 24.0f, 2.0f / 24.0f, 8.0f / 24.0f, 16.0f / 24.0f});
}

TEST_CASE("A slice wider than the source is clamped to it", "[gameui]")
{
    UISprite sprite = kFrame;
    sprite.mSlice = UIEdges{30, 0, 30, 0};
    const std::vector<UIDrawCommand> commands = SpriteCommands(UIRect{0, 0, 100, 10}, sprite);

    // the left slice takes all 24 texels and the right none
    REQUIRE(commands.size() == 2);
    CheckRect(commands[0].mRect, UIRect{0, 0, 24, 10});
    CheckRect(commands[0].mUV, UIRect{0, 0, 1, 1});
    CheckRect(commands[1].mRect, UIRect{24, 0, 76, 10});
}

TEST_CASE("A null sprite or an empty rect records nothing, and pieces outside the clip are culled", "[gameui]")
{
    CHECK(SpriteCommands(UIRect{0, 0, 100, 100}, UISprite{}).empty());
    CHECK(SpriteCommands(UIRect{0, 0, 0, 100}, kFrame).empty());

    UIDrawList list;
    list.Reset(100, 100);
    list.PushClip(UIRect{0, 0, 30, 100});
    list.Sprite(UIRect{0, 0, 200, 50}, kFrame);
    list.PopClip();

    // only the left column reaches the clip
    REQUIRE(list.Commands().size() == 6);
    for (const UIDrawCommand &command : list.Commands())
        CHECK(command.mRect.x < 30.0f);
}

TEST_CASE("PushOpacity fades every kind of command, nested pushes multiply, and PopOpacity restores", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);
    CHECK(list.CurrentOpacity() == 1.0f);

    list.PushOpacity(0.5f);
    list.Rect(UIRect{0, 0, 10, 10}, UIColor{1, 1, 1, 0.8f});
    list.PushOpacity(0.5f);
    list.Texture(UIRect{0, 0, 10, 10}, UITexture{1});
    list.Text(0, 0, "faded", kUIWhite);
    CHECK(list.OpacityDepth() == 2);
    list.PopOpacity();
    list.Sprite(UIRect{0, 0, 10, 10}, UISprite{.mTexture = UITexture{1}, .mTextureWidth = 4, .mTextureHeight = 4,
                                               .mSource = UIRect{0, 0, 4, 4}});
    list.PopOpacity();
    list.Rect(UIRect{0, 0, 10, 10}, kUIWhite);

    REQUIRE(list.Commands().size() == 5);
    CHECK(list.Commands()[0].mColor.a == Catch::Approx(0.4f));
    CHECK(list.Commands()[1].mColor.a == Catch::Approx(0.25f));
    CHECK(list.Commands()[2].mColor.a == Catch::Approx(0.25f));
    CHECK(list.Commands()[3].mColor.a == Catch::Approx(0.5f));
    CHECK(list.Commands()[4].mColor.a == 1.0f);
    CHECK(list.Commands()[0].mColor.r == 1.0f);
    CHECK(list.OpacityDepth() == 0);
}

TEST_CASE("PushOpacity clamps to 0 and 1, and Reset drops a push left open", "[gameui]")
{
    UIDrawList list;
    list.Reset(100, 100);

    list.PushOpacity(3.0f);
    CHECK(list.CurrentOpacity() == 1.0f);
    list.PushOpacity(-1.0f);
    CHECK(list.CurrentOpacity() == 0.0f);

    list.Reset(100, 100);
    CHECK(list.OpacityDepth() == 0);
    CHECK(list.CurrentOpacity() == 1.0f);
}

TEST_CASE("Shapes record their points, width and angles, and a shape with nothing to draw records nothing", "[gameui][shapes]")
{
    UIDrawList list;
    list.Reset(200, 200);
    list.Line({10, 10}, {50, 10}, kUIWhite, UIStroke{.mWidth = 3});
    const UIPoint triangle[] = {{0, 0}, {40, 0}, {20, 30}};
    list.Polygon(triangle, kUIWhite);
    list.Polyline(triangle, kUIWhite, {}, true);
    list.StrokeCircle({100, 100}, 20, kUIWhite, UIStroke{.mWidth = 2});
    list.FillCircle({100, 100}, 20, kUIWhite);
    list.Arc({100, 100}, 30, 0.0f, 1.0f, kUIWhite, UIStroke{.mWidth = 4});
    list.Pie({100, 100}, 30, 0.5f, 2.0f, kUIWhite);
    list.StrokeRect({10, 10, 50, 20}, kUIWhite, UIStroke{.mWidth = 2}, 4);

    const auto commands = list.Commands();
    REQUIRE(commands.size() == 8);
    CHECK(commands[0].mKind == UIDrawCommandKind::Polyline);
    CHECK(commands[0].mThickness == 3.0f);
    CHECK_FALSE(commands[0].mClosed);
    REQUIRE(list.PointsOf(commands[0]).size() == 2);
    CheckPoint(list.PointsOf(commands[0])[1], {50, 10});
    CHECK(commands[1].mKind == UIDrawCommandKind::Polygon);
    CHECK(list.PointsOf(commands[1]).size() == 3);
    CHECK(commands[2].mClosed);
    CHECK(commands[3].mKind == UIDrawCommandKind::Circle);
    CHECK(commands[3].mThickness == 2.0f);
    CHECK(commands[3].mRect.width == 20.0f);
    CHECK(commands[4].mThickness == 0.0f);
    CHECK(commands[5].mKind == UIDrawCommandKind::Arc);
    CHECK(commands[5].mAngleTo == 1.0f);
    CHECK(commands[6].mKind == UIDrawCommandKind::Arc);
    CHECK(commands[6].mThickness == 0.0f);
    CHECK(commands[6].mAngleFrom == 0.5f);
    CHECK(commands[7].mKind == UIDrawCommandKind::RectOutline);
    CHECK(commands[7].mRounding == 4.0f);
    CHECK(list.PointsOf(commands[7]).empty());

    // too few points, no radius, no width, no sweep
    list.Reset(200, 200);
    list.Polyline(std::span(triangle, 1), kUIWhite);
    list.Polygon(std::span(triangle, 2), kUIWhite);
    list.FillCircle({10, 10}, 0, kUIWhite);
    list.Line({0, 0}, {10, 10}, kUIWhite, UIStroke{.mWidth = 0});
    list.Arc({10, 10}, 5, 1.0f, 1.0f, kUIWhite);
    list.StrokeRect({0, 0, 0, 10}, kUIWhite);
    CHECK(list.Commands().empty());
}

TEST_CASE("A full turn is a circle, as an arc or a pie", "[gameui][shapes]")
{
    UIDrawList list;
    list.Reset(200, 200);
    list.Arc({50, 50}, 10, 0.0f, kTurn, kUIWhite, UIStroke{.mWidth = 2});
    list.Pie({50, 50}, 10, 1.0f, 1.0f - kTurn, kUIWhite);
    REQUIRE(list.Commands().size() == 2);
    CHECK(list.Commands()[0].mKind == UIDrawCommandKind::Circle);
    CHECK(list.Commands()[0].mThickness == 2.0f);
    CHECK(list.Commands()[1].mKind == UIDrawCommandKind::Circle);
    CHECK(list.Commands()[1].mThickness == 0.0f);
}

TEST_CASE("A dashed stroke is recorded as one line a dash, moved along by its offset", "[gameui][shapes]")
{
    UIDrawList list;
    list.Reset(200, 200);
    list.Line({0, 0}, {10, 0}, kUIWhite, UIStroke{.mWidth = 1, .mDash = 2, .mGap = 3});
    std::vector<std::vector<UIPoint>> dashes = Lines(list);
    REQUIRE(dashes.size() == 2);
    CheckPoint(dashes[0].front(), {0, 0});
    CheckPoint(dashes[0].back(), {2, 0});
    CheckPoint(dashes[1].front(), {5, 0});
    CheckPoint(dashes[1].back(), {7, 0});

    // a dash already 1 along: 0 to 1, 4 to 6 and 9 to the end
    list.Reset(200, 200);
    list.Line({0, 0}, {10, 0}, kUIWhite, UIStroke{.mWidth = 1, .mDash = 2, .mGap = 3, .mDashOffset = 1});
    dashes = Lines(list);
    REQUIRE(dashes.size() == 3);
    CheckPoint(dashes[0].back(), {1, 0});
    CheckPoint(dashes[1].front(), {4, 0});
    CheckPoint(dashes[2].front(), {9, 0});
    CheckPoint(dashes[2].back(), {10, 0});

    // round a 10 unit square, a dash a side, and one that turns a corner keeps it
    list.Reset(200, 200);
    list.StrokeRect({0, 0, 10, 10}, kUIWhite, UIStroke{.mWidth = 1, .mDash = 5, .mGap = 5});
    CHECK(Lines(list).size() == 4);
    list.Reset(200, 200);
    list.StrokeRect({0, 0, 10, 10}, kUIWhite, UIStroke{.mWidth = 1, .mDash = 5, .mGap = 5, .mDashOffset = 2});
    dashes = Lines(list);
    REQUIRE(dashes.size() == 5);
    CHECK(dashes[0].size() == 2);
    REQUIRE(dashes[1].size() == 3);
    CheckPoint(dashes[1][1], {10, 0});

    // dashed circles and arcs
    list.Reset(200, 200);
    list.StrokeCircle({100, 100}, 20, kUIWhite, UIStroke{.mWidth = 1, .mDash = 4, .mGap = 4});
    CHECK(Lines(list).size() > 10);
    list.Reset(200, 200);
    list.Arc({100, 100}, 20, 0.0f, kTurn * 0.25f, kUIWhite, UIStroke{.mWidth = 1, .mDash = 4, .mGap = 4});
    CHECK(Lines(list).size() == 4);

    // too fine to see, drawn solid
    list.Reset(200, 200);
    list.Line({0, 0}, {100, 0}, kUIWhite, UIStroke{.mWidth = 1, .mDash = 0.001f, .mGap = 0.001f});
    dashes = Lines(list);
    REQUIRE(dashes.size() == 1);
    CHECK(dashes[0].size() == 2);
}

TEST_CASE("What is drawn keeps the transform it was drawn under, and pushes nest inside one another", "[gameui][transform]")
{
    UIDrawList list;
    list.Reset(200, 200);
    const UITransform move = UITransform::Translate(10, 0);
    const UITransform turn = UITransform::Rotate(kTurn * 0.25f, {50, 50});

    list.PushTransform(move);
    list.PushTransform(turn);
    CHECK(list.TransformDepth() == 2);
    list.Rect({40, 40, 20, 20}, kUIWhite);
    list.PopTransform();
    list.Text(5, 5, "moved", kUIWhite);
    list.PopTransform();
    list.Line({0, 0}, {10, 10}, kUIWhite);
    CHECK(list.TransformDepth() == 0);

    const auto commands = list.Commands();
    REQUIRE(commands.size() == 3);
    CHECK(list.TransformOf(commands[0]) == move * turn);
    CHECK(list.TransformOf(commands[1]) == move);
    CHECK(list.TransformOf(commands[2]).IsIdentity());

    list.PushTransform(move);
    list.Reset(200, 200);
    CHECK(list.TransformDepth() == 0);
    CHECK(list.CurrentTransform().IsIdentity());
}

TEST_CASE("Culling and clips go through the transform", "[gameui][transform]")
{
    UIDrawList list;
    list.Reset(100, 100);
    list.Rect({200, 0, 10, 10}, kUIWhite);
    CHECK(list.Commands().empty());

    // moved into the area, it is kept
    list.PushTransform(UITransform::Translate(-150, 0));
    list.Rect({200, 0, 10, 10}, kUIWhite);
    list.FillCircle({200, 50}, 5, kUIWhite);
    list.PopTransform();
    CHECK(list.Commands().size() == 2);

    // a clip pushed under a transform is its bounds in the area
    list.PushTransform(UITransform::Translate(50, 50));
    list.PushClip({0, 0, 10, 10});
    CheckRect(list.CurrentClip(), {50, 50, 10, 10});
    list.PopClip();
    list.PushTransform(UITransform::Rotate(kTurn * 0.125f, {5, 5}));
    list.PushClip({0, 0, 10, 10});
    CHECK(list.CurrentClip().width > 10.0f);
    list.PopClip();
    list.PopTransform();
    list.PopTransform();
}

TEST_CASE("A gradient is kept with its shape, and opacity fades the shape's colour", "[gameui][shapes]")
{
    UIDrawList list;
    list.Reset(200, 200);
    const UIGradient gradient{.mFrom = {0, 0}, .mTo = {100, 0}, .mFromColor = {1, 0, 0, 1}, .mToColor = {0, 0, 1, 1}};
    list.PushOpacity(0.5f);
    list.Rect({0, 0, 100, 10}, kUIWhite, 0.0f, gradient);
    list.Line({0, 20}, {100, 20}, kUIWhite, UIStroke{.mWidth = 2, .mDash = 10, .mGap = 10}, gradient);
    list.FillCircle({50, 50}, 10, UIColor{1, 1, 1, 1});
    list.PopOpacity();

    const auto commands = list.Commands();
    REQUIRE(commands.size() == 7);
    REQUIRE(list.GradientOf(commands[0]) != nullptr);
    CHECK(*list.GradientOf(commands[0]) == gradient);
    CHECK(commands[0].mColor.a == Catch::Approx(0.5f));
    // each dash takes the gradient, still read along the whole line
    for (size_t i = 1; i < 6; ++i)
        CHECK(*list.GradientOf(commands[i]) == gradient);
    CHECK(list.GradientOf(commands[6]) == nullptr);
    CHECK(commands[6].mColor.a == Catch::Approx(0.5f));
    CHECK_FALSE(UIGradient{}.IsSet());
}
