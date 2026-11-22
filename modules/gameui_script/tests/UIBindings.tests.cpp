/**
 * @file UIBindings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests the ui object a script's OnUI is given, driven through a real UIContext with no backend.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui_script/UIBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>
#include <script/components/ScriptRef.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/EngineMode.h>
#include <core/ecs/World.h>
#include <input/InputMap.h>
#include <input/InputState.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

using namespace mir;

namespace
{
    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            {"clicked", FieldKind::Bool},
            {"held", FieldKind::Bool},
            {"toggled", FieldKind::Bool},
            {"slid", FieldKind::Float},
            {"x", FieldKind::Float},
            {"y", FieldKind::Float},
            {"w", FieldKind::Float},
            {"h", FieldKind::Float},
            {"calls", FieldKind::Int},
            {"focusTaken", FieldKind::Bool},
        };
        return ComponentRegistry::Instance().RegisterRuntime("UIBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbe(World &world, Entity entity, std::string_view field)
    {
        const ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    constexpr UIRect kArea{ 0, 0, 640, 480 };

    struct Fixture
    {
        World world;
        ScriptHost host;
        Registration bindings = AddBindings(host, &RegisterUIBindings);
        std::shared_ptr<UIScriptFrame> frame = std::make_shared<UIScriptFrame>();
        UIContext ui;
        UIPointer pointer;

        Fixture()
        {
            world.EmplaceResource<InputState>();
            world.EmplaceResource<InputMap>();
            world.EmplaceResource<EngineModeState>();
            SetEngineMode(world, EngineMode::Play);
        }

        Entity Spawn(std::string_view name, std::string_view source, bool started = true)
        {
            if (!host.LoadScriptSource(name, source))
                FAIL("the script did not load");
            const Entity entity = world.CreateEntity();
            ProbeOps().AddDefault(world, entity);
            ScriptRef ref;
            ref.instanceRef = host.CreateInstance(name);
            ref.started = started;
            world.AddComponent<ScriptRef>(entity, ref);
            return entity;
        }

        void Frame()
        {
            ui.BeginFrame(kArea, world.Resource<InputState>(), world.Resource<InputMap>(), pointer);
            DrawScriptScreens(host, world, frame, ui, 1.0f / 60.0f);
            ui.EndFrame();
        }

        void Click(float x, float y)
        {
            pointer = UIPointer{ x, y, false };
            Frame();
            pointer.mDown = true;
            Frame();
            pointer.mDown = false;
            Frame();
        }

        std::optional<UIRect> TextAt(std::string_view text)
        {
            for (const UIDrawCommand &command : ui.DrawList().Commands())
                if (command.mKind == UIDrawCommandKind::Text && ui.DrawList().TextOf(command) == text)
                    return command.mRect;
            return std::nullopt;
        }
    };

    constexpr std::string_view kButtonSource = R"LUA(
local T = {}
function T.NewInstanceData() return { x = 100, calls = 0 } end
function T.OnUI(self, world, entity, ui, dt)
    local result = ui:button("go", { x = self.x, y = 100, w = 120, h = 40 }, "Go")
    if result.clicked then world:set(entity, "UIBindingsProbe", "clicked", true) end
    world:set(entity, "UIBindingsProbe", "held", result.held)
    self.calls = self.calls + 1
    world:set(entity, "UIBindingsProbe", "calls", self.calls)
end
return T
)LUA";
}

TEST_CASE("A script's OnUI draws a button the pointer clicks, once OnStart has run and only in Play", "[gameui][script]")
{
    Fixture f;
    const Entity started = f.Spawn("ui_button", kButtonSource);
    const Entity waiting = f.Spawn("ui_button", kButtonSource, false);

    f.Click(150, 120);
    CHECK(ReadProbe<bool>(f.world, started, "clicked"));
    CHECK(ReadProbe<int32_t>(f.world, started, "calls") == 3);
    CHECK(ReadProbe<int32_t>(f.world, waiting, "calls") == 0);
    CHECK(f.host.ErrorCount() == 0);

    SetEngineMode(f.world, EngineMode::Edit);
    f.Frame();
    CHECK(ReadProbe<int32_t>(f.world, started, "calls") == 3);
    CHECK(f.ui.DrawList().Commands().empty());

    // paused is still Play, so a game's own pause menu keeps drawing
    SetEngineMode(f.world, EngineMode::Play);
    SetPlayState(f.world, PlayState::Paused);
    f.Frame();
    CHECK(ReadProbe<int32_t>(f.world, started, "calls") == 4);
}

TEST_CASE("Two entities running one script do not share a widget", "[gameui][script]")
{
    Fixture f;
    const Entity first = f.Spawn("ui_button", kButtonSource);
    const Entity second = f.Spawn("ui_button", kButtonSource);
    // both name their button "go", in one place
    f.pointer = UIPointer{ 150, 120, false };
    f.Frame();
    f.pointer.mDown = true;
    f.Frame();
    f.Frame();

    // the one drawn last is on top and takes the press; the other is not held with it
    CHECK(ReadProbe<bool>(f.world, first, "held") != ReadProbe<bool>(f.world, second, "held"));
    CHECK(f.ui.State().Collisions() == 0);
}

TEST_CASE("toggle and slider hand back the value, and layout helpers return rects", "[gameui][script]")
{
    Fixture f;
    const Entity entity = f.Spawn("ui_values", R"LUA(
local T = {}
function T.NewInstanceData() return { on = false, level = 0.25 } end
function T.OnUI(self, world, entity, ui, dt)
    local area = ui:bounds()
    local panel = ui:anchor(area, "top_left", 300, 200)
    local column = ui:column(ui:inset(panel, 10), 5)
    column:next(20)
    local second = column:next(30)
    world:set(entity, "UIBindingsProbe", "x", second.x)
    world:set(entity, "UIBindingsProbe", "y", second.y)
    world:set(entity, "UIBindingsProbe", "w", second.w)
    world:set(entity, "UIBindingsProbe", "h", second.h)

    self.on = ui:toggle("sound", { x = 100, y = 300, w = 200, h = 30 }, "Sound", self.on)
    self.level = ui:slider("level", { x = 100, y = 400, w = 200, h = 30 }, self.level, 0, 1, { step = 0.25 })
    world:set(entity, "UIBindingsProbe", "toggled", self.on)
    world:set(entity, "UIBindingsProbe", "slid", self.level)
end
return T
)LUA");

    f.Frame();
    CHECK(ReadProbe<float>(f.world, entity, "x") == Catch::Approx(10.0f));
    CHECK(ReadProbe<float>(f.world, entity, "y") == Catch::Approx(35.0f));
    CHECK(ReadProbe<float>(f.world, entity, "w") == Catch::Approx(280.0f));
    CHECK(ReadProbe<float>(f.world, entity, "h") == Catch::Approx(30.0f));
    CHECK_FALSE(ReadProbe<bool>(f.world, entity, "toggled"));
    CHECK(ReadProbe<float>(f.world, entity, "slid") == Catch::Approx(0.25f));

    f.Click(150, 315);
    CHECK(ReadProbe<bool>(f.world, entity, "toggled"));
    CHECK(f.host.ErrorCount() == 0);
}

TEST_CASE("A failed OnUI leaves the frame balanced and the next script still draws", "[gameui][script]")
{
    Fixture f;
    f.Spawn("ui_broken", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:with_clip({ x = 0, y = 0, w = 50, h = 50 }, function()
        ui:with_opacity(0.5, function()
            error("boom")
        end)
    end)
end
return T
)LUA");
    f.Spawn("ui_after", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:label("after", { x = 300, y = 300, w = 100, h = 30 }, "After")
end
return T
)LUA");

    f.ui.BeginFrame(kArea, f.world.Resource<InputState>(), f.world.Resource<InputMap>(), f.pointer);
    DrawScriptScreens(f.host, f.world, f.frame, f.ui, 0.0f);
    CHECK(f.ui.DrawList().ClipDepth() == 0);
    CHECK(f.ui.DrawList().OpacityDepth() == 0);
    f.ui.EndFrame();

    CHECK(f.host.ErrorCount() == 1);
    REQUIRE(f.TextAt("After").has_value());
}

TEST_CASE("A bad argument fails the OnUI, and a ui kept past its OnUI cannot be used", "[gameui][script]")
{
    Fixture f;
    f.Spawn("ui_bad_rect", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:button("go", { 1, 2, 3, 4 }, "Go")
end
return T
)LUA");
    const Entity keeper = f.Spawn("ui_keeper", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    self.kept = ui
end
function T.OnUpdate(self, world, entity, dt)
    self.kept:panel({ x = 0, y = 0, w = 10, h = 10 })
end
return T
)LUA");
    f.Spawn("ui_bad_option", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:button("go", { x = 0, y = 0, w = 10, h = 10 }, "Go", { colour = "red" })
end
return T
)LUA");

    f.Frame();
    CHECK(f.host.ErrorCount() == 2);

    // ScriptSystem's isolation, as in App
    CommandBuffer commands;
    try
    {
        f.host.CallOnUpdate(f.world, commands, keeper, f.world.GetComponent<ScriptRef>(keeper)->instanceRef, 0.0f);
    }
    catch (const std::exception &)
    {
    }
    CHECK(f.host.ErrorCount() == 3);
}

TEST_CASE("ui:set_focus says whether it took, and is refused while a Shell screen holds the input", "[gameui][script]")
{
    Fixture f;
    const Entity entity = f.Spawn("ui_focus", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:button("go", { x = 0, y = 0, w = 10, h = 10 }, "Go")
    world:set(entity, "UIBindingsProbe", "focusTaken", ui:set_focus("go"))
end
return T
)LUA");

    f.Frame();
    CHECK(ReadProbe<bool>(f.world, entity, "focusTaken"));

    // as GameUIModule runs the Game layer under a Shell screen that holds the input
    const UIId before = f.ui.State().Focus();
    f.ui.BeginFrame(kArea, f.world.Resource<InputState>(), f.world.Resource<InputMap>(), f.pointer);
    f.ui.SetFocus(UIId("menu"));
    f.ui.SetWidgetsDisabled(true);
    DrawScriptScreens(f.host, f.world, f.frame, f.ui, 0.0f);
    f.ui.SetWidgetsDisabled(false);
    f.ui.EndFrame();
    CHECK_FALSE(ReadProbe<bool>(f.world, entity, "focusTaken"));
    CHECK(f.ui.State().Focus() == UIId("menu"));
    CHECK(f.ui.State().Focus() != before);
    CHECK(f.host.ErrorCount() == 0);
}

TEST_CASE("Text options style one widget's text and are put back after it", "[gameui][script]")
{
    Fixture f;
    // as if ui:font had loaded it, since this fixture has no assets
    f.frame->mFonts.emplace("fonts/hud.ttf", UIFont{ 5 });
    const Entity entity = f.Spawn("ui_text", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    local font = ui:font("fonts/hud.ttf")
    ui:label("speed", { x = 100, y = 100, w = 200, h = 40 }, "SPD", {
        font = font, size = 24, color = { 1, 0, 0, 1 }, align = "right",
        shadow = { x = 2, y = 3, color = { r = 0, g = 0, b = 0, a = 0.5 } }, outline = true,
    })
    ui:label("plain", { x = 100, y = 200, w = 200, h = 40 }, "Plain")
    local w, h = ui:measure_text("abcd", nil, 10)
    world:set(entity, "UIBindingsProbe", "w", w)
    world:set(entity, "UIBindingsProbe", "h", h)
end
return T
)LUA");

    const UIStyle before = f.ui.Style();
    f.Frame();
    REQUIRE(f.host.ErrorCount() == 0);

    std::vector<UIDrawCommand> speed;
    std::vector<UIDrawCommand> plain;
    for (const UIDrawCommand &command : f.ui.DrawList().Commands())
    {
        if (command.mKind != UIDrawCommandKind::Text)
            continue;
        (f.ui.DrawList().TextOf(command) == "SPD" ? speed : plain).push_back(command);
    }

    // a shadow, eight outline copies and the text, all in the font and size
    REQUIRE(speed.size() == 10);
    for (const UIDrawCommand &command : speed)
    {
        CHECK(command.mFont == UIFont{ 5 });
        CHECK(command.mFontSize == 24.0f);
    }
    CHECK(speed.front().mColor == UIColor{ 0, 0, 0, 0.5f });
    CHECK(speed.front().mRect.x == Catch::Approx(speed.back().mRect.x + 2.0f));
    CHECK(speed.back().mColor == UIColor{ 1, 0, 0, 1 });
    // right aligned: 3 characters estimated at half of 24 each
    CHECK(speed.back().mRect.x == Catch::Approx(300.0f - before.mPadding - 36.0f));

    REQUIRE(plain.size() == 1);
    CHECK(plain[0].mFont == UIFont{});
    CHECK(plain[0].mFontSize == before.mFontSize);
    CHECK(plain[0].mColor == before.mTextColor);

    CHECK(f.ui.Style().mFont == before.mFont);
    CHECK(f.ui.Style().mTextAlign == before.mTextAlign);
    CHECK(f.ui.Style().mTextOutlineColor == before.mTextOutlineColor);
    CHECK(ReadProbe<float>(f.world, entity, "w") == Catch::Approx(20.0f));
    CHECK(ReadProbe<float>(f.world, entity, "h") == Catch::Approx(10.0f));
}

TEST_CASE("ui:font is nil for a font that does not load, and a bad text option fails the OnUI", "[gameui][script]")
{
    Fixture f;
    const Entity entity = f.Spawn("ui_no_font", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    world:set(entity, "UIBindingsProbe", "clicked", ui:font("fonts/missing.ttf") == nil)
end
return T
)LUA");
    f.Spawn("ui_bad_align", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:label("a", { x = 0, y = 0, w = 10, h = 10 }, "A", { align = "middle" })
end
return T
)LUA");
    f.Spawn("ui_text_on_slider", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:slider("s", { x = 0, y = 0, w = 10, h = 10 }, 0, 0, 1, { size = 20 })
end
return T
)LUA");
    f.Spawn("ui_bad_font", R"LUA(
local T = {}
function T.NewInstanceData() return {} end
function T.OnUI(self, world, entity, ui, dt)
    ui:button("b", { x = 0, y = 0, w = 10, h = 10 }, "B", { font = "fonts/hud.ttf" })
end
return T
)LUA");

    const UIStyle before = f.ui.Style();
    f.Frame();
    CHECK(ReadProbe<bool>(f.world, entity, "clicked"));
    CHECK(f.host.ErrorCount() == 3);
    CHECK(f.ui.Style().mFont == before.mFont);
    CHECK(f.ui.Style().mTextAlign == before.mTextAlign);
}
