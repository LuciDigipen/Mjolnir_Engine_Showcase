/**
 * @file PauseMenu.cpp
 * @author Quy Dong Ta
 * @brief The menu a game pauses into: Resume, Restart, Audio, Controls and Quit
 *        over a dimmed screen, and the screens behind it, driven by gamepad,
 *        mouse or keyboard, built on gameui's public API alone.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "gameshell/PauseMenu.h"

#include "ShellPrompts.h"
#include <gameui/UILayout.h>
#include <gameui/UIWidgets.h>
#include <input/DefaultInputMap.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <iterator>
#include <span>
#include <string_view>

namespace mir
{
    namespace
    {
        constexpr UIId kResume("pause_resume");
        constexpr UIId kRestart("pause_restart");
        constexpr UIId kAudio("pause_audio");
        constexpr UIId kQuit("pause_quit");

        constexpr UIId kMusicVolume("audio_music_volume");
        constexpr UIId kMusicMute("audio_music_mute");
        constexpr UIId kSfxVolume("audio_sfx_volume");
        constexpr UIId kSfxMute("audio_sfx_mute");
        constexpr UIId kRestoreDefaults("audio_restore_defaults");
        constexpr UIId kAudioBack("audio_back");

        constexpr UIId kControls("pause_controls");
        constexpr UIId kInvertMouse("controls_invert_mouse");
        constexpr UIId kInvertGamepad("controls_invert_gamepad");
        constexpr UIId kControlsDefaults("controls_restore_defaults");
        constexpr UIId kControlsBack("controls_back");

        constexpr UIId kAudioColumn[] = {kMusicVolume, kMusicMute, kSfxVolume, kSfxMute, kRestoreDefaults, kAudioBack};
        constexpr UIId kControlsColumn[] = {kInvertMouse, kInvertGamepad, kControlsDefaults, kControlsBack};

        // only the entries whose screens have options behind them
        struct MainColumn
        {
            std::array<UIId, 5> mIds{};
            size_t mCount = 0;

            void Add(UIId id) { mIds[mCount++] = id; }
            std::span<const UIId> Span() const { return {mIds.data(), mCount}; }
        };

        struct ChannelRows
        {
            AudioChannel mChannel;
            std::string_view mName;
            std::string_view mMuteText;
            UIId mLabel;
            UIId mVolume;
            UIId mPercent;
            UIId mMute;
        };

        constexpr ChannelRows kMusicRows{AudioChannel::Music, "Music", "Mute music", UIId("audio_music_label"),
                                         kMusicVolume, UIId("audio_music_percent"), kMusicMute};
        constexpr ChannelRows kSfxRows{AudioChannel::Sfx, "SFX", "Mute SFX", UIId("audio_sfx_label"), kSfxVolume,
                                       UIId("audio_sfx_percent"), kSfxMute};

        constexpr UIColor kDim{0.0f, 0.0f, 0.0f, 0.55f};

        constexpr float kWidth = 360.0f;
        constexpr float kAudioWidth = 420.0f;
        constexpr float kPadding = 16.0f;
        constexpr float kGap = 12.0f;
        constexpr float kRow = 40.0f;
        constexpr float kPromptRow = 24.0f;
        constexpr float kPromptGap = 16.0f;
        constexpr float kChannelNameWidth = 64.0f;
        constexpr float kPercentWidth = 56.0f;
        constexpr float kSliderHeight = 24.0f;
        constexpr float kVolumeStep = 0.1f;

        // the title, then rows, then the prompts
        constexpr float PanelHeight(size_t rows)
        {
            const float count = static_cast<float>(rows + 1);
            return kRow * count + kGap * count + kPromptRow + 2.0f * kPadding;
        }

        // up and down to the neighbours in the column, wrapping round
        UIWidgetOptions InColumn(std::span<const UIId> column, UIId id)
        {
            const size_t count = column.size();
            const size_t index = static_cast<size_t>(std::find(column.begin(), column.end(), id) - column.begin());
            return {.mLinks = {column[(index + count - 1) % count], column[(index + 1) % count], UIId{}, UIId{}}};
        }

        UIRect CenteredPanel(UIContext &ui, float width, size_t rows)
        {
            const UIRect safe = ui.SafeArea();
            const UIRect panel =
                Anchor(safe, kUICenter, std::min(width, safe.width), std::min(PanelHeight(rows), safe.height));
            Panel(ui, panel);
            return panel;
        }

        // right-aligned, so back is cut first and Select sits to its left
        void DrawPrompts(UIContext &ui, const UIRect &row)
        {
            UIStack prompts = UIStack::Row(row, kPromptGap);
            shell::DrawPrompt(ui, prompts, kUICancelAction, "Back");
            shell::DrawPrompt(ui, prompts, kUIConfirmAction, "Select");
        }

        // the name, the slider and its percent on one row, the mute under them
        void DrawChannel(UIContext &ui, UIStack &rows, IAudioOptions &audio, const ChannelRows &channel)
        {
            UIStack row = UIStack::Row(rows.Next(kRow), kGap);
            Label(ui, channel.mLabel, row.Next(kChannelNameWidth), channel.mName);
            const UIRect percent = row.Last(kPercentWidth);
            const UIRect track = row.Rest();

            float volume = audio.Volume(channel.mChannel);
            if (Slider(ui, channel.mVolume, Anchor(track, kUILeft, track.width, std::min(kSliderHeight, track.height)),
                       volume, 0.0f, 1.0f, kVolumeStep, InColumn(kAudioColumn, channel.mVolume))
                    .mChanged)
                audio.SetVolume(channel.mChannel, volume);
            Label(ui, channel.mPercent, percent, std::format("{}%", std::lround(volume * 100.0f)));

            bool muted = audio.Muted(channel.mChannel);
            if (Toggle(ui, channel.mMute, rows.Next(kRow), channel.mMuteText, muted,
                       InColumn(kAudioColumn, channel.mMute))
                    .mChanged)
                audio.SetMuted(channel.mChannel, muted);
        }
    } // namespace

    PauseMenuAction PauseMenu::Draw(UIContext &ui, bool canOpen)
    {
        if (!canOpen)
        {
            mOpen = false;
            mOpenRequested = false;
            return PauseMenuAction::None;
        }

        PauseMenuAction action = PauseMenuAction::None;
        if (!mOpen)
        {
            if (!ui.PausePressed() && !mOpenRequested)
                return PauseMenuAction::None;

            mOpen = true;
            mOpenRequested = false;
            mScreen = Screen::Main;
            ui.State().SetFocus(kResume);
            action = PauseMenuAction::Opened;
        }

        // Escape is both, so it backs out of a screen a step at a time; Start alone resumes from anywhere
        else if (mScreen != Screen::Main && ui.CancelPressed())
        {
            BackToMain(ui);
        }

        else if (ui.PausePressed() || ui.CancelPressed())
        {
            action = PauseMenuAction::Resumed;
        }

        ui.HoldInput();


        ui.DrawList().Rect(ui.Bounds(), kDim);

        if (mScreen == Screen::Audio && mAudio != nullptr)
            DrawAudio(ui);
        else if (mScreen == Screen::Controls && mControls != nullptr)
            DrawControls(ui);
        else if (const PauseMenuAction chosen = DrawMain(ui); chosen != PauseMenuAction::None)
            action = chosen;

        if (action == PauseMenuAction::Resumed || action == PauseMenuAction::Restart ||
            action == PauseMenuAction::Quit)
            mOpen = false;
        return action;
    }

    PauseMenuAction PauseMenu::DrawMain(UIContext &ui)
    {
        MainColumn entries;
        entries.Add(kResume);
        entries.Add(kRestart);
        if (mAudio != nullptr)
            entries.Add(kAudio);
        if (mControls != nullptr)
            entries.Add(kControls);
        entries.Add(kQuit);
        const std::span<const UIId> column = entries.Span();

        UIStack rows = UIStack::Column(Inset(CenteredPanel(ui, kWidth, column.size()), kPadding), kGap);
        Label(ui, UIId("pause_title"), rows.Next(kRow), "Paused");

        //a column that wraps
        PauseMenuAction action = PauseMenuAction::None;
        if (Button(ui, kResume, rows.Next(kRow), "Resume", InColumn(column, kResume)).mClicked)
            action = PauseMenuAction::Resumed;
        if (Button(ui, kRestart, rows.Next(kRow), "Restart", InColumn(column, kRestart)).mClicked)
            action = PauseMenuAction::Restart;
        if (mAudio != nullptr && Button(ui, kAudio, rows.Next(kRow), "Audio", InColumn(column, kAudio)).mClicked)
        {
            mScreen = Screen::Audio;
            ui.State().SetFocus(kMusicVolume);
        }
        if (mControls != nullptr &&
            Button(ui, kControls, rows.Next(kRow), "Controls", InColumn(column, kControls)).mClicked)
        {
            mScreen = Screen::Controls;
            ui.State().SetFocus(kInvertMouse);
        }
        if (Button(ui, kQuit, rows.Next(kRow), "Quit", InColumn(column, kQuit)).mClicked)
            action = PauseMenuAction::Quit;

        DrawPrompts(ui, rows.Next(kPromptRow));
        return action;
    }

    void PauseMenu::DrawAudio(UIContext &ui)
    {
        UIStack rows = UIStack::Column(Inset(CenteredPanel(ui, kAudioWidth, std::size(kAudioColumn)), kPadding), kGap);
        Label(ui, UIId("audio_title"), rows.Next(kRow), "Audio");

        DrawChannel(ui, rows, *mAudio, kMusicRows);
        DrawChannel(ui, rows, *mAudio, kSfxRows);

        if (Button(ui, kRestoreDefaults, rows.Next(kRow), "Restore defaults", InColumn(kAudioColumn, kRestoreDefaults))
                .mClicked)
            mAudio->RestoreDefaults();
        if (Button(ui, kAudioBack, rows.Next(kRow), "Back", InColumn(kAudioColumn, kAudioBack)).mClicked)
            BackToMain(ui);

        DrawPrompts(ui, rows.Next(kPromptRow));
    }

    void PauseMenu::DrawControls(UIContext &ui)
    {
        UIStack rows =
            UIStack::Column(Inset(CenteredPanel(ui, kAudioWidth, std::size(kControlsColumn)), kPadding), kGap);
        Label(ui, UIId("controls_title"), rows.Next(kRow), "Controls");

        bool invertMouse = mControls->InvertY(LookDevice::Mouse);
        if (Toggle(ui, kInvertMouse, rows.Next(kRow), "Invert mouse Y", invertMouse,
                   InColumn(kControlsColumn, kInvertMouse))
                .mChanged)
            mControls->SetInvertY(LookDevice::Mouse, invertMouse);

        bool invertGamepad = mControls->InvertY(LookDevice::Gamepad);
        if (Toggle(ui, kInvertGamepad, rows.Next(kRow), "Invert gamepad Y", invertGamepad,
                   InColumn(kControlsColumn, kInvertGamepad))
                .mChanged)
            mControls->SetInvertY(LookDevice::Gamepad, invertGamepad);

        if (Button(ui, kControlsDefaults, rows.Next(kRow), "Restore defaults",
                   InColumn(kControlsColumn, kControlsDefaults))
                .mClicked)
            mControls->RestoreDefaults();
        if (Button(ui, kControlsBack, rows.Next(kRow), "Back", InColumn(kControlsColumn, kControlsBack)).mClicked)
            BackToMain(ui);

        DrawPrompts(ui, rows.Next(kPromptRow));
    }

    // onto the entry the screen was opened from
    void PauseMenu::BackToMain(UIContext &ui)
    {
        ui.State().SetFocus(mScreen == Screen::Controls ? kControls : kAudio);
        mScreen = Screen::Main;
    }
} // namespace mir
