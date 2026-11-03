/**
 * @file BuildSettingsPanel.cpp
 * @author Quy Dong Ta
 * @brief The "Build Settings" window: profile, player template, output folder and Run after build.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/panels/BuildSettingsPanel.h>

#include <editortheme/EditorTheme.h>
#include <project/PlayerInfo.h>

#include <imgui.h>

#include <cctype>
#include <cfloat>
#include <cstdio>
#include <optional>
#include <system_error>

namespace mir
{
    namespace
    {
        constexpr double kRecheckSeconds = 2.0;

        std::string &TemplateText(EditorPreferences &preferences, BuildProfile profile)
        {
            return profile == BuildProfile::Development ? preferences.buildDevelopmentTemplate
                                                        : preferences.buildReleaseTemplate;
        }

        template <std::size_t N>
        void CopyText(char (&buffer)[N], const std::string &text)
        {
            std::snprintf(buffer, N, "%s", text.c_str());
        }

        // the build's messages start lowercase, as part of a longer line
        void Line(const std::string &message, std::optional<ImVec4> color = std::nullopt)
        {
            std::string text = message;
            if (!text.empty())
                text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
            text += '.';

            if (color)
                ImGui::PushStyleColor(ImGuiCol_Text, *color);
            ImGui::TextWrapped("%s", text.c_str());
            if (color)
                ImGui::PopStyleColor();
        }
    }

    void BuildSettingsPanel::Load(EditorPreferences &preferences, const BuildGame &game)
    {
        mProfile = ReadBuildProfile(game.mProjectSettingsPath);
        CopyText(mTemplatePath, TemplateText(preferences, mProfile));
        const auto output = preferences.buildOutputs.find(BuildOutputKey(game));
        CopyText(mOutputPath, output != preferences.buildOutputs.end() ? output->second : std::string());
        mStatusMessage.clear();
        mCheckedDir.clear();
        mLoaded = true;
    }

    void BuildSettingsPanel::DrawTemplateStatus(const std::filesystem::path &dir, const BuildGame &game)
    {
        const double now = ImGui::GetTime();
        if (dir != mCheckedDir || mProfile != mCheckedProfile || now - mCheckedAt > kRecheckSeconds)
        {
            mCheck = CheckPlayerTemplate(dir, mProfile, game.mEditorInfo);
            mCheckedDir = dir;
            mCheckedProfile = mProfile;
            mCheckedAt = now;
        }

        if (mCheck.mProblem.empty())
        {
            const auto it = mCheck.mInfo.find("config");
            const std::string config = it != mCheck.mInfo.end() && it->is_string() ? it->get<std::string>() : "unknown";
            ImGui::Text("Engine %s, %s build", PlayerInfoVersion(mCheck.mInfo).c_str(), config.c_str());

            std::string features;
            for (const PlayerFeature &feature : kPlayerFeatures)
            {
                const std::optional<bool> on = PlayerInfoFlag(mCheck.mInfo, feature.mKey);
                features += (features.empty() ? "" : ", ") + std::string(feature.mOption) +
                    (on ? (*on ? " ON" : " OFF") : " unknown");
            }
            ImGui::TextWrapped("%s", features.c_str());

            if (mCheck.mWarnings.empty())
                Line("it matches the editor", EditorTheme::Color(ThemeColor::Success));
        }
        else
        {
            Line(mCheck.mProblem, EditorTheme::Color(StatusColor::Danger));
        }
        for (const std::string &warning : mCheck.mWarnings)
            Line(warning, EditorTheme::Color(StatusColor::Warning));

        if (mCheck.mProblem.empty() && mCheck.mWarnings.empty())
            return;

        // building the player needs the MSVC environment, so the editor only shows how
        ImGui::Spacing();
        ImGui::TextWrapped("To make it, run these from a Developer Command Prompt for VS in %s:",
            game.mSourceRoot.string().c_str());
        std::string commands;
        for (const std::string &command : PlayerInstallCommands(game.mSourceRoot, dir, mProfile))
            commands += (commands.empty() ? "" : "\n") + command;
        const ImGuiStyle &style = ImGui::GetStyle();
        const float wrap = ImGui::GetContentRegionAvail().x - style.FramePadding.x * 2.0f;
        const float height = ImGui::CalcTextSize(commands.c_str(), nullptr, false, wrap).y +
            ImGui::GetTextLineHeight() + style.FramePadding.y * 2.0f;
        ImGui::InputTextMultiline("##install", commands.data(), commands.size() + 1, ImVec2(-FLT_MIN, height),
            ImGuiInputTextFlags_ReadOnly | ImGuiInputTextFlags_WordWrap);
        if (ImGui::Button("Copy commands"))
            ImGui::SetClipboardText(commands.c_str());

        std::error_code ec;
        if (std::filesystem::exists(dir, ec))
            ImGui::TextDisabled("cmake --install adds to what the folder holds, so delete it first.");
    }

    bool BuildSettingsPanel::Draw(EditorPreferences &preferences, const BuildGame &game)
    {
        if (!mLoaded)
            Load(preferences, game);
        bool save = false;

        ImGui::SeparatorText("Profile");
        const char *profileNames[] = { "Development", "Release" };
        int profile = static_cast<int>(mProfile);
        if (ImGui::Combo("Profile", &profile, profileNames, IM_ARRAYSIZE(profileNames)))
        {
            const BuildProfile chosen = static_cast<BuildProfile>(profile);
            if (WriteBuildProfile(game.mProjectSettingsPath, chosen))
            {
                mProfile = chosen;
                CopyText(mTemplatePath, TemplateText(preferences, mProfile));
                mStatusMessage.clear();
            }
            else
            {
                mStatusMessage = "Profile not saved - see log.";
            }
        }
        ImGui::TextDisabled(mProfile == BuildProfile::Development ? "Packages the game-debug player."
                                                                  : "Packages the game-release player.");
        ImGui::TextDisabled("Saved in project_settings.json, so the whole team builds the same way.");

        ImGui::SeparatorText("Player template");
        ImGui::TextWrapped("Paths are saved for this user only. Empty is the default; a relative path is from the "
                           "checkout.");
        const std::string templateHint =
            CheckoutPath(DefaultPlayerTemplate(game.mSourceRoot, mProfile), game.mSourceRoot);
        ImGui::InputTextWithHint("Template folder", templateHint.c_str(), mTemplatePath, sizeof(mTemplatePath));
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            TemplateText(preferences, mProfile) = mTemplatePath;
            save = true;
        }
        DrawTemplateStatus(PlayerTemplateDir(preferences, game, mProfile), game);

        ImGui::SeparatorText("Output");
        const std::string outputHint = CheckoutPath(DefaultBuildOutput(game), game.mSourceRoot);
        ImGui::InputTextWithHint("Output folder", outputHint.c_str(), mOutputPath, sizeof(mOutputPath));
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            if (mOutputPath[0] == '\0')
                preferences.buildOutputs.erase(BuildOutputKey(game));
            else
                preferences.buildOutputs[BuildOutputKey(game)] = mOutputPath;
            save = true;
        }
        const std::filesystem::path exe = BuildOutputDir(preferences, game) / (game.mName + ".exe");
        ImGui::TextWrapped("For this game only. %s will start the build.",
            CheckoutPath(exe, game.mSourceRoot).c_str());

        save |= ImGui::Checkbox("Run after build", &preferences.buildRunAfter);
        ImGui::SetItemTooltip("Starts the game once a build succeeds");

        if (!mStatusMessage.empty())
            ImGui::TextColored(EditorTheme::Color(StatusColor::Danger), "%s", mStatusMessage.c_str());
        return save;
    }
}
