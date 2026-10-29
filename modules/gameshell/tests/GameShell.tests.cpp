/**
 * @file GameShell.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for gameshell: the pause menu opened, navigated and closed by
 *        the gamepad alone, the mouse alone and the keyboard alone, game input
 *        held while it is open, the HUD out of the stick's reach, and both in
 *        the editor's viewport and fullscreen, and the Audio screen the same
 *        way. Raw input goes through InputSystem and the default bindings, as
 *        in App.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameshell/GameShell.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/World.h>
#include <gameui/UIContext.h>
#include <input/DefaultInputMap.h>
#include <input/InputMap.h>
#include <input/InputState.h>
#include <input/InputSystem.h>
#include <window/Window.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>


#include <optional>
#include <string>
#include <string_view>

using namespace mir;

namespace
{
    // the editor's scene viewport, somewhere inside the window, and game mode
    constexpr UIRect kViewport{300, 80, 1280, 720};
    constexpr UIRect kFullscreen{0, 0, 1920, 1080};

    constexpr UIId kResume("pause_resume");
    constexpr UIId kRestart("pause_restart");
    constexpr UIId kAudio("pause_audio");
    constexpr UIId kQuit("pause_quit");
    constexpr UIId kHudPause("hud_pause");

    constexpr UIId kMusicVolume("audio_music_volume");
    constexpr UIId kMusicMute("audio_music_mute");
    constexpr UIId kSfxVolume("audio_sfx_volume");
    constexpr UIId kRestoreDefaults("audio_restore_defaults");
    constexpr UIId kAudioBack("audio_back");

    struct TestChannel
    {
        float mVolume = 0.5f;
        bool mMuted = false;

        bool operator==(const TestChannel &) const = default;
    };

    constexpr TestChannel kProjectDefault{0.8f, false};

    // the audio module stood in for: what the screen set, and the project's defaults to go back to
    class TestAudioOptions final : public IAudioOptions
    {
    public:
        float Volume(AudioChannel channel) const override { return Of(channel).mVolume; }
        void SetVolume(AudioChannel channel, float volume) override { Of(channel).mVolume = volume; }
        bool Muted(AudioChannel channel) const override { return Of(channel).mMuted; }
        void SetMuted(AudioChannel channel, bool muted) override { Of(channel).mMuted = muted; }
        void RestoreDefaults() override { mMusic = mSfx = kProjectDefault; }

        TestChannel mMusic;
        TestChannel mSfx;

    private:
        TestChannel &Of(AudioChannel channel) { return channel == AudioChannel::Music ? mMusic : mSfx; }
        const TestChannel &Of(AudioChannel channel) const { return channel == AudioChannel::Music ? mMusic : mSfx; }
    };

    class TestWindow final : public Window
    {
    public:
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        NativeWindowHandle NativeWindow() const override { return {}; }
        uint32_t Width() const override { return 1920; }
        uint32_t Height() const override { return 1080; }
        const RawInputSnapshot &RawInput() const override { return mRaw; }

        RawInputSnapshot mRaw;
    };

    class Fixture
    {
    public:
        explicit Fixture(const UIRect &area = kFullscreen) : mArea(area)
        {
            // a game's own action, beside the defaults
            mMap.AddAction("Fire", InputActionType::Button)
                .bindings.push_back(InputBinding{.device = DeviceKind::Keyboard, .key = Key::F});
        }

        Fixture(const Fixture &) = delete;
        Fixture &operator=(const Fixture &) = delete;

        // one frame as App runs it: the raw input resolved into actions, then
        // the game UI's frame with the pointer read from the same raw input
        GameShellAction Frame(bool playing = true, float dt = 1.0f / 60.0f)
        {
            mSystem.OnUpdate(mContext);

            const RawInputSnapshot &raw = mWindow.mRaw;
            mUI.BeginFrame(mArea, mInput, mMap,
                           UIPointer{static_cast<float>(raw.mouseX), static_cast<float>(raw.mouseY),
                                     raw.IsDown(MouseButton::Left)});
            const GameShellAction action = mShell.Draw(mUI, playing, dt);
            mUI.EndFrame();
            return action;
        }

        // down for a frame and up for the next; what the down frame did
        GameShellAction Tap(Key key)
        {
            SetKey(key, true);
            const GameShellAction action = Frame();
            SetKey(key, false);
            Frame();
            return action;
        }

        GameShellAction Tap(GamepadButton button)
        {
            PadButton(button, true);
            const GameShellAction action = Frame();
            PadButton(button, false);
            Frame();
            return action;
        }

        // pressed and released over where the pointer is; what the release did
        GameShellAction Click(MouseButton button = MouseButton::Left)
        {
            SetMouse(button, true);
            Frame();
            SetMouse(button, false);
            return Frame();
        }

        // over where text was drawn last frame, and a frame for hover to settle
        void PointAt(std::string_view text)
        {
            const std::optional<UIRect> drawn = TextAt(text);
            REQUIRE(drawn.has_value());
            mWindow.mRaw.mouseX = mArea.x + drawn->x + 4.0;
            mWindow.mRaw.mouseY = mArea.y + drawn->y + 4.0;
            Frame();
        }

        // in the area's own coordinates, as drawn text gives them
        void MoveTo(float x, float y)
        {
            mWindow.mRaw.mouseX = mArea.x + x;
            mWindow.mRaw.mouseY = mArea.y + y;
        }

        // pushed for a frame and let go the next
        void Flick(GamepadAxis axis, float value)
        {
            mWindow.mRaw.gamepads[0].connected = true;
            mWindow.mRaw.gamepads[0].axes[static_cast<size_t>(axis)] = value;
            Frame();
            mWindow.mRaw.gamepads[0].axes[static_cast<size_t>(axis)] = 0.0f;
            Frame();
        }

        std::optional<UIRect> TextAt(std::string_view text)
        {
            for (const UIDrawCommand &command : mUI.DrawList().Commands())
                if (command.mKind == UIDrawCommandKind::Text && mUI.DrawList().TextOf(command) == text)
                    return command.mRect;
            return std::nullopt;
        }

        void SetKey(Key key, bool down) { mWindow.mRaw.keys[static_cast<size_t>(key)] = down; }
        void SetMouse(MouseButton button, bool down) { mWindow.mRaw.mouseButtons[static_cast<size_t>(button)] = down; }
        void PadButton(GamepadButton button, bool down)
        {
            mWindow.mRaw.gamepads[0].connected = true;
            mWindow.mRaw.gamepads[0].buttons[static_cast<size_t>(button)] = down;
        }

        GameShell &Shell() { return mShell; }
        UIContext &UI() { return mUI; }
        InputState &Input() { return mInput; }
        UIId Focus() const { return mUI.State().Focus(); }

    private:
        UIRect mArea;
        TestWindow mWindow;
        World mWorld;
        CommandBuffer mCommands;
        InputMap &mMap = mWorld.EmplaceResource<InputMap>(DefaultInputMap());
        InputState &mInput = mWorld.EmplaceResource<InputState>();
        InputSystem mSystem{mWindow};
        SystemContext mContext{mWorld, mCommands, 1.0f / 60.0f, 0.0, 0};
        UIContext mUI;
        GameShell mShell;
    };
} // namespace

TEST_CASE("The pause menu opens, navigates and closes by the gamepad alone", "[gameshell]")
{
    Fixture f;
    f.Frame();

    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Pause);
    REQUIRE(f.Shell().MenuOpen());
    CHECK(f.Input().ActiveDevice() == DeviceKind::Gamepad);
    CHECK(f.Focus() == kResume);

    // down the column and round, by the D-pad
    f.Tap(GamepadButton::DpadDown);
    CHECK(f.Focus() == kRestart);
    f.Tap(GamepadButton::DpadDown);
    CHECK(f.Focus() == kQuit);
    f.Tap(GamepadButton::DpadDown);
    CHECK(f.Focus() == kResume);
    f.Tap(GamepadButton::DpadUp);
    CHECK(f.Focus() == kQuit);
    f.Tap(GamepadButton::DpadUp);
    CHECK(f.Focus() == kRestart);

    // B backs out
    CHECK(f.Tap(GamepadButton::B) == GameShellAction::Resume);
    CHECK_FALSE(f.Shell().MenuOpen());

    // Start closes what it opened, and A clicks the focused button
    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Pause);
    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Resume);
    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Pause);
    CHECK(f.Tap(GamepadButton::A) == GameShellAction::Resume);
    CHECK_FALSE(f.Shell().MenuOpen());
}

TEST_CASE("The pause menu opens, navigates and closes by the keyboard alone", "[gameshell]")
{
    Fixture f;
    f.Frame();

    CHECK(f.Tap(Key::Escape) == GameShellAction::Pause);
    REQUIRE(f.Shell().MenuOpen());
    CHECK(f.Input().ActiveDevice() == DeviceKind::Keyboard);
    CHECK(f.Focus() == kResume);

    f.Tap(Key::Down);
    CHECK(f.Focus() == kRestart);
    f.Tap(Key::Up);
    f.Tap(Key::Up);
    CHECK(f.Focus() == kQuit);

    // Escape backs out, Enter clicks
    CHECK(f.Tap(Key::Escape) == GameShellAction::Resume);
    CHECK(f.Tap(Key::Escape) == GameShellAction::Pause);
    f.Tap(Key::Down);
    CHECK(f.Tap(Key::Enter) == GameShellAction::Restart);
    CHECK_FALSE(f.Shell().MenuOpen());
}

TEST_CASE("The pause menu opens, navigates and closes by the mouse alone, in the editor's viewport",
          "[gameshell]")
{
    Fixture f(kViewport);
    f.Frame();

    // the HUD's pause button, clicked
    f.PointAt("Pause");
    CHECK(f.UI().State().Hot() == kHudPause);
    CHECK(f.Click() == GameShellAction::Pause);
    REQUIRE(f.Shell().MenuOpen());
    CHECK(f.Input().ActiveDevice() == DeviceKind::Mouse);

    // hovering a button focuses it, but the highlight stays hidden under the mouse
    f.PointAt("Restart");
    CHECK(f.Focus() == kRestart);
    CHECK_FALSE(f.UI().FocusVisible());

    f.PointAt("Resume");
    CHECK(f.Click() == GameShellAction::Resume);
    CHECK_FALSE(f.Shell().MenuOpen());

    // and the right button backs out, on its press
    f.PointAt("Pause");
    CHECK(f.Click() == GameShellAction::Pause);
    f.SetMouse(MouseButton::Right, true);
    CHECK(f.Frame() == GameShellAction::Resume);
    f.SetMouse(MouseButton::Right, false);
    f.Frame();
    CHECK_FALSE(f.Shell().MenuOpen());

    f.PointAt("Pause");
    CHECK(f.Click() == GameShellAction::Pause);
    f.PointAt("Quit");
    CHECK(f.Click() == GameShellAction::Quit);
}

TEST_CASE("Game input is held while the menu is open, by gameui, without touching SetUiCapture", "[gameshell]")
{
    Fixture f;
    f.SetKey(Key::F, true);
    f.Frame();
    CHECK(f.Input().ForGame("Fire").down);
    CHECK_FALSE(f.Input().GameInputHeld());

    CHECK(f.Tap(Key::Escape) == GameShellAction::Pause);
    f.Frame();
    CHECK(f.UI().HoldsInput());
    CHECK(f.Input().GameInputHeld());
    CHECK_FALSE(f.Input().ForGame("Fire").down);

    // the key is still read, and every device still reaches the menu
    CHECK(f.Input().Get("Fire").down);
    CHECK_FALSE(f.Input().UiCapturesKeyboard());
    CHECK_FALSE(f.Input().UiCapturesMouse());
    f.Tap(Key::Down);
    CHECK(f.Focus() == kRestart);

    // the frame that closes it still holds; the next gives the game its input back
    CHECK(f.Tap(Key::Escape) == GameShellAction::Resume);
    CHECK_FALSE(f.Input().GameInputHeld());
    CHECK(f.Input().ForGame("Fire").down);
}

TEST_CASE("While the game plays, the stick and confirm belong to it, not to the HUD", "[gameshell]")
{
    Fixture f;
    f.Frame();
    REQUIRE(f.TextAt("Pause").has_value());

    CHECK(f.Tap(GamepadButton::DpadDown) == GameShellAction::None);
    CHECK(f.Tap(GamepadButton::A) == GameShellAction::None);
    CHECK(f.Focus().IsNull());
    CHECK_FALSE(f.Shell().MenuOpen());
    CHECK_FALSE(f.Input().GameInputHeld());
}

TEST_CASE("Outside Play nothing is drawn or opened, and Stop closes an open menu", "[gameshell]")
{
    Fixture f;
    f.SetKey(Key::Escape, true);
    CHECK(f.Frame(false) == GameShellAction::None);
    CHECK(f.UI().DrawList().Commands().empty());
    CHECK_FALSE(f.Shell().MenuOpen());
    f.SetKey(Key::Escape, false);
    f.Frame(false);

    f.Frame();
    CHECK(f.Tap(Key::Escape) == GameShellAction::Pause);
    CHECK(f.Frame(false) == GameShellAction::None);
    CHECK_FALSE(f.Shell().MenuOpen());
    CHECK_FALSE(f.Input().GameInputHeld());
}

TEST_CASE("The menu and HUD draw inside the editor's viewport and fullscreen, at any safe area", "[gameshell]")
{
    for (const UIRect area : {kViewport, kFullscreen})
    {
        for (const UIEdges insets : {UIEdges{}, UIEdges{area.width * 0.05f, area.height * 0.05f, area.width * 0.05f,
                                                          area.height * 0.05f}})
        {
            Fixture f(area);
            f.UI().SetSafeAreaInsets(insets);
            f.Frame();
            f.Tap(Key::Escape);
            REQUIRE(f.Shell().MenuOpen());

            const UIRect bounds = f.UI().Bounds();
            const UIRect safe = f.UI().SafeArea();
            for (const UIDrawCommand &command : f.UI().DrawList().Commands())
            {
                // the dim alone covers the whole area; everything else keeps to the safe area
                const UIRect &inside = command.mRect == bounds ? bounds : safe;
                CHECK(command.mRect.x >= inside.x);
                CHECK(command.mRect.y >= inside.y);
                CHECK(command.mRect.Right() <= inside.Right());
                CHECK(command.mRect.Bottom() <= inside.Bottom());
            }

            // the HUD top right, the menu across the middle
            const std::optional<UIRect> hud = f.TextAt("Pause");
            const std::optional<UIRect> title = f.TextAt("Paused");
            REQUIRE(hud.has_value());
            REQUIRE(title.has_value());
            CHECK(hud->x > safe.x + safe.width * 0.75f);
            CHECK(hud->y < safe.y + safe.height * 0.15f);
            CHECK(title->x > safe.x + safe.width * 0.25f);
            CHECK(title->x < safe.x + safe.width * 0.5f);
        }
    }
}

TEST_CASE("The HUD's clock counts play, stops in the menu and starts again on Restart and on a new Play",
          "[gameshell]")
{
    Fixture f;
    f.Frame(true, 1.0f);
    f.Frame(true, 1.0f);
    CHECK(f.Shell().PlaySeconds() == Catch::Approx(2.0f));
    CHECK(f.TextAt("00:02").has_value());

    // the frame Escape opens the menu on still counts; the frames in it do not
    f.SetKey(Key::Escape, true);
    CHECK(f.Frame(true, 1.0f) == GameShellAction::Pause);
    CHECK(f.Shell().PlaySeconds() == Catch::Approx(3.0f));
    f.SetKey(Key::Escape, false);
    f.Frame(true, 5.0f);
    f.SetKey(Key::Down, true);
    f.Frame(true, 5.0f);
    f.SetKey(Key::Down, false);
    f.Frame(true, 5.0f);
    CHECK(f.Shell().PlaySeconds() == Catch::Approx(3.0f));

    f.SetKey(Key::Enter, true);
    CHECK(f.Frame(true, 5.0f) == GameShellAction::Restart);
    CHECK(f.Shell().PlaySeconds() == Catch::Approx(0.0f));
    f.SetKey(Key::Enter, false);
    f.Frame(true, 3.0f);
    CHECK(f.Shell().PlaySeconds() == Catch::Approx(3.0f));

    // a new Play starts from zero
    f.Frame(false, 1.0f);
    f.Frame(true, 1.0f);
    CHECK(f.Shell().PlaySeconds() == Catch::Approx(1.0f));
}

TEST_CASE("Without audio options the menu has no Audio entry; with them it does", "[gameshell]")
{
    Fixture f;
    f.Frame();
    f.Tap(Key::Escape);
    REQUIRE(f.Shell().MenuOpen());
    CHECK_FALSE(f.TextAt("Audio").has_value());

    TestAudioOptions audio;
    f.Shell().SetAudioOptions(&audio);
    f.Frame();
    CHECK(f.TextAt("Audio").has_value());
}

TEST_CASE("The Audio screen opens, adjusts and backs out by the gamepad alone", "[gameshell]")
{
    Fixture f;
    TestAudioOptions audio;
    f.Shell().SetAudioOptions(&audio);
    f.Frame();

    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Pause);
    f.Tap(GamepadButton::DpadDown);
    f.Tap(GamepadButton::DpadDown);
    CHECK(f.Focus() == kAudio);

    // A opens it on its first control, and the menu stays open
    CHECK(f.Tap(GamepadButton::A) == GameShellAction::None);
    REQUIRE(f.Shell().AudioScreenOpen());
    CHECK(f.Focus() == kMusicVolume);

    // the D-pad steps the slider, and the volume follows on the same frame
    f.Tap(GamepadButton::DpadRight);
    CHECK(audio.mMusic.mVolume == Catch::Approx(0.6f).margin(1e-4));
    f.Tap(GamepadButton::DpadLeft);
    f.Tap(GamepadButton::DpadLeft);
    CHECK(audio.mMusic.mVolume == Catch::Approx(0.4f).margin(1e-4));

    // and the left stick does both
    f.Flick(GamepadAxis::LeftY, 1.0f);
    CHECK(f.Focus() == kMusicMute);
    f.Tap(GamepadButton::A);
    CHECK(audio.mMusic.mMuted);
    f.Tap(GamepadButton::DpadDown);
    CHECK(f.Focus() == kSfxVolume);
    f.Flick(GamepadAxis::LeftX, -1.0f);
    CHECK(audio.mSfx.mVolume == Catch::Approx(0.4f).margin(1e-4));

    f.Tap(GamepadButton::DpadDown);
    f.Tap(GamepadButton::DpadDown);
    CHECK(f.Focus() == kRestoreDefaults);
    f.Tap(GamepadButton::A);
    CHECK(audio.mMusic == kProjectDefault);
    CHECK(audio.mSfx == kProjectDefault);

    // B goes back a step, onto Audio, without resuming
    CHECK(f.Tap(GamepadButton::B) == GameShellAction::None);
    CHECK(f.Shell().MenuOpen());
    CHECK_FALSE(f.Shell().AudioScreenOpen());
    CHECK(f.Focus() == kAudio);

    // Start resumes from inside it, and the menu opens on Resume next time
    f.Tap(GamepadButton::A);
    REQUIRE(f.Shell().AudioScreenOpen());
    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Resume);
    CHECK_FALSE(f.Shell().MenuOpen());
    CHECK(f.Tap(GamepadButton::Start) == GameShellAction::Pause);
    CHECK_FALSE(f.Shell().AudioScreenOpen());
    CHECK(f.Focus() == kResume);
}

TEST_CASE("The Audio screen steps a volume through ten steps by the keyboard alone, and Escape backs out a step",
          "[gameshell]")
{
    Fixture f;
    TestAudioOptions audio;
    f.Shell().SetAudioOptions(&audio);
    f.Frame();

    CHECK(f.Tap(Key::Escape) == GameShellAction::Pause);
    f.Tap(Key::Up);
    CHECK(f.Focus() == kQuit);
    f.Tap(Key::Up);
    CHECK(f.Focus() == kAudio);
    CHECK(f.Tap(Key::Enter) == GameShellAction::None);
    REQUIRE(f.Shell().AudioScreenOpen());
    CHECK(f.Focus() == kMusicVolume);

    // up to full and no further, then ten steps down to silent
    for (int i = 0; i < 6; ++i)
        f.Tap(Key::Right);
    CHECK(audio.mMusic.mVolume == Catch::Approx(1.0f));
    float last = audio.mMusic.mVolume;
    for (int step = 0; step < 10; ++step)
    {
        f.Tap(Key::Left);
        CHECK(audio.mMusic.mVolume < last);
        last = audio.mMusic.mVolume;
    }
    CHECK(audio.mMusic.mVolume == Catch::Approx(0.0f).margin(1e-4));

    // down the column to Back, and round
    for (int i = 0; i < 5; ++i)
        f.Tap(Key::Down);
    CHECK(f.Focus() == kAudioBack);
    f.Tap(Key::Down);
    CHECK(f.Focus() == kMusicVolume);
    f.Tap(Key::Up);
    CHECK(f.Focus() == kAudioBack);

    CHECK(f.Tap(Key::Enter) == GameShellAction::None);
    CHECK(f.Shell().MenuOpen());
    CHECK_FALSE(f.Shell().AudioScreenOpen());
    CHECK(f.Focus() == kAudio);

    // Escape is Back and Pause both: a step back first, then the game resumes
    f.Tap(Key::Enter);
    REQUIRE(f.Shell().AudioScreenOpen());
    CHECK(f.Tap(Key::Escape) == GameShellAction::None);
    CHECK(f.Shell().MenuOpen());
    CHECK_FALSE(f.Shell().AudioScreenOpen());
    CHECK(f.Tap(Key::Escape) == GameShellAction::Resume);
    CHECK_FALSE(f.Shell().MenuOpen());
}

TEST_CASE("The Audio screen is used by the mouse alone, in the editor's viewport", "[gameshell]")
{
    Fixture f(kViewport);
    TestAudioOptions audio;
    f.Shell().SetAudioOptions(&audio);
    f.Frame();

    f.PointAt("Pause");
    CHECK(f.Click() == GameShellAction::Pause);
    f.PointAt("Audio");
    CHECK(f.Click() == GameShellAction::None);
    REQUIRE(f.Shell().AudioScreenOpen());
    // the click lands on the release, so the screen draws from the frame after
    f.Frame();

    // the music slider lies between its name and its percent; dragged, it follows past either end
    const std::optional<UIRect> name = f.TextAt("Music");
    const std::optional<UIRect> percent = f.TextAt("50%");
    REQUIRE(name.has_value());
    REQUIRE(percent.has_value());
    const float y = percent->y + 4.0f;

    f.MoveTo((name->x + percent->x) * 0.5f, y);
    f.SetMouse(MouseButton::Left, true);
    f.Frame();
    f.Frame();
    CHECK(audio.mMusic.mVolume > 0.0f);
    CHECK(audio.mMusic.mVolume < 1.0f);
    f.MoveTo(kViewport.width, y);
    f.Frame();
    CHECK(audio.mMusic.mVolume == Catch::Approx(1.0f));
    f.MoveTo(name->x, y);
    f.Frame();
    CHECK(audio.mMusic.mVolume == Catch::Approx(0.0f).margin(1e-4));
    f.SetMouse(MouseButton::Left, false);
    f.Frame();
    CHECK(f.TextAt("0%").has_value());

    f.PointAt("Mute SFX");
    f.Click();
    CHECK(audio.mSfx.mMuted);
    f.PointAt("Restore defaults");
    f.Click();
    CHECK(audio.mMusic == kProjectDefault);
    CHECK(audio.mSfx == kProjectDefault);

    // the right button backs out a step, and so does the Back button
    f.SetMouse(MouseButton::Right, true);
    CHECK(f.Frame() == GameShellAction::None);
    f.SetMouse(MouseButton::Right, false);
    f.Frame();
    CHECK(f.Shell().MenuOpen());
    CHECK_FALSE(f.Shell().AudioScreenOpen());

    f.PointAt("Audio");
    f.Click();
    REQUIRE(f.Shell().AudioScreenOpen());
    f.Frame();
    f.PointAt("Back");
    CHECK(f.Click() == GameShellAction::None);
    CHECK(f.Shell().MenuOpen());
    CHECK_FALSE(f.Shell().AudioScreenOpen());
}

TEST_CASE("The Audio screen draws inside the editor's viewport and fullscreen, at any safe area", "[gameshell]")
{
    for (const UIRect area : {kViewport, kFullscreen})
    {
        for (const UIEdges insets : {UIEdges{}, UIEdges{area.width * 0.05f, area.height * 0.05f, area.width * 0.05f,
                                                          area.height * 0.05f}})
        {
            Fixture f(area);
            TestAudioOptions audio;
            f.Shell().SetAudioOptions(&audio);
            f.UI().SetSafeAreaInsets(insets);
            f.Frame();
            f.Tap(Key::Escape);
            f.Tap(Key::Up);
            f.Tap(Key::Up);
            f.Tap(Key::Enter);
            REQUIRE(f.Shell().AudioScreenOpen());

            const UIRect bounds = f.UI().Bounds();
            const UIRect safe = f.UI().SafeArea();
            for (const UIDrawCommand &command : f.UI().DrawList().Commands())
            {
                const UIRect &inside = command.mRect == bounds ? bounds : safe;
                CHECK(command.mRect.x >= inside.x);
                CHECK(command.mRect.y >= inside.y);
                CHECK(command.mRect.Right() <= inside.Right());
                CHECK(command.mRect.Bottom() <= inside.Bottom());
            }

            for (const std::string_view text : {"Audio", "Music", "Mute music", "SFX", "Mute SFX", "Restore defaults"})
                CHECK(f.TextAt(text).has_value());
        }
    }
}
