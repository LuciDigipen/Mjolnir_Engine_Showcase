/**
 * @file BuildSettingsPanel.cpp
 * @author Quy Dong Ta
 * @brief The "Build Settings" window: profile, build options, game build template and whether Build compiles it,
 *        output folder, Run after build, and Build.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/panels/BuildSettingsPanel.h>

#include <editortheme/EditorTheme.h>
#include <project/GameBuildInfo.h>
#include <serial/JsonFile.h>

#include <imgui.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

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
            if (!text.ends_with('.'))
                text += '.';

            if (color)
                ImGui::PushStyleColor(ImGuiCol_Text, *color);
            ImGui::TextWrapped("%s", text.c_str());
            if (color)
                ImGui::PopStyleColor();
        }

        void Note(const std::string &text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", text.c_str());
            ImGui::PopStyleColor();
        }

        std::string Joined(const std::vector<std::string> &items)
        {
            std::string text;
            for (std::size_t i = 0; i < items.size(); ++i)
                text += (i == 0 ? "" : i + 1 == items.size() ? " and " : ", ") + items[i];
            return text;
        }
    }

    void BuildSettingsPanel::Load(EditorPreferences &preferences, const BuildGame &game)
    {
        mProfile = ReadBuildProfile(game.mProjectSettingsPath);
        CopyText(mTemplatePath, TemplateText(preferences, mProfile));
        const auto output = preferences.buildOutputs.find(BuildOutputKey(game));
        CopyText(mOutputPath, output != preferences.buildOutputs.end() ? output->second : std::string());

        mChoices = ReadGameBuildChoices(game.mProjectSettingsPath);
        mChoicesReadAt = ImGui::GetTime();
        const std::optional<nlohmann::json> editor = ReadJsonFile(game.mEditorInfo);
        mEditorInfo = editor && editor->is_object() ? *editor : nlohmann::json();
        std::error_code ec;
        mHasSoundBanks = !game.mSoundBankDir.empty() && std::filesystem::is_directory(game.mSoundBankDir, ec) &&
            !std::filesystem::is_empty(game.mSoundBankDir, ec);

        mStatusMessage.clear();
        mCheckedDir.clear();
        mLoaded = true;
    }

    void BuildSettingsPanel::SaveChoice(const BuildGame &game, std::string_view key, std::optional<bool> on)
    {
        if (!WriteGameBuildChoice(game.mProjectSettingsPath, key, on))
        {
            mStatusMessage = "Build option not saved - see log.";
            return;
        }
        mChoices = ReadGameBuildChoices(game.mProjectSettingsPath);
        mStatusMessage.clear();
        // the template is checked against the choices
        mCheckedDir.clear();
    }

    void BuildSettingsPanel::DrawOptions(const BuildGame &game, bool compile)
    {
        // a pull can change them while the window is open
        if (ImGui::GetTime() - mChoicesReadAt > kRecheckSeconds)
        {
            mChoices = ReadGameBuildChoices(game.mProjectSettingsPath);
            mChoicesReadAt = ImGui::GetTime();
        }

        ImGui::SeparatorText("Build options");
        std::optional<bool> audio;
        for (const GameBuildFeature &feature : kGameBuildFeatures)
        {
            const std::string key(feature.mKey);
            const std::string label(feature.mLabel);
            // copied, since saving reads the choices again
            const auto found = mChoices.mOn.find(key);
            const std::optional<bool> chosen =
                found != mChoices.mOn.end() ? std::optional<bool>(found->second) : std::nullopt;
            const std::optional<bool> editor = GameBuildInfoFlag(mEditorInfo, feature.mKey);
            if (feature.mKey == "audio")
                audio = chosen ? chosen : editor;

            ImGui::PushID(key.c_str());
            bool on = chosen.value_or(editor.value_or(false));
            if (ImGui::Checkbox(label.c_str(), &on))
                SaveChoice(game, key, on);
            ImGui::SetItemTooltip("%s (%s)", std::string(feature.mAbout).c_str(), std::string(feature.mOption).c_str());

            ImGui::SameLine();
            if (!chosen)
            {
                ImGui::TextDisabled(editor ? "Editor Default" : "The editor's is unknown, so the preset decides");
            }
            else
            {
                if (editor && *editor != *chosen)
                {
                    ImGui::TextDisabled("the editor has %s %s", label.c_str(), *editor ? "ON" : "OFF");
                    ImGui::SameLine();
                }
                if (ImGui::SmallButton("Reset"))
                    SaveChoice(game, key, std::nullopt);
                ImGui::SetItemTooltip(editor ? (*editor ? "Follow the editor again, which has it ON"
                                                        : "Follow the editor again, which has it OFF")
                                             : "Follow the editor again");
            }
            ImGui::PopID();
        }

        for (const std::string &problem : mChoices.mProblems)
            Line(problem, EditorTheme::Color(StatusColor::Warning));
        if (mHasSoundBanks && audio == false)
            Line("the game has SoundBanks, which the packaged game cannot play with Audio OFF",
                EditorTheme::Color(StatusColor::Warning));

        Note("Saved in project_settings.json for the whole team. An option left alone follows the editor.");
        if (!compile)
            Note("They take effect when Build compiles the game build; until then the template is checked against them.");
    }

    void BuildSettingsPanel::DrawTemplateStatus(const std::filesystem::path &dir, const BuildGame &game, bool compile)
    {
        const double now = ImGui::GetTime();
        if (dir != mCheckedDir || mProfile != mCheckedProfile || now - mCheckedAt > kRecheckSeconds)
        {
            mCheck = CheckGameBuildTemplate(dir, mProfile, game.mEditorInfo, mChoices);
            std::error_code ec;
            mTemplateMissing = !std::filesystem::exists(dir, ec) || std::filesystem::is_empty(dir, ec);
            mCheckedDir = dir;
            mCheckedProfile = mProfile;
            mCheckedAt = now;
        }

        if (compile && mTemplateMissing)
        {
            ImGui::TextDisabled("Not compiled yet; the first Build compiles it.");
            return;
        }
        if (compile && !mCheck.mProblem.empty())
        {
            Line(mCheck.mProblem + "; Build replaces the folder only when it holds a game build template",
                EditorTheme::Color(StatusColor::Warning));
            return;
        }

        if (mCheck.mProblem.empty())
        {
            const auto it = mCheck.mInfo.find("config");
            const std::string config = it != mCheck.mInfo.end() && it->is_string() ? it->get<std::string>() : "unknown";
            ImGui::Text("Engine %s, %s build", GameBuildInfoVersion(mCheck.mInfo).c_str(), config.c_str());

            std::string features;
            for (const GameBuildFeature &feature : kGameBuildFeatures)
            {
                const std::optional<bool> on = GameBuildInfoFlag(mCheck.mInfo, feature.mKey);
                features += (features.empty() ? "" : ", ") + std::string(feature.mOption) +
                    (on ? (*on ? " ON" : " OFF") : " unknown");
            }
            ImGui::TextWrapped("%s", features.c_str());

            // the compile makes it match, so a mismatch now does not matter
            if (compile)
            {
                const std::vector<std::string> changed = ChangedGameBuildOptions(mCheck.mInfo, mChoices, mEditorInfo);
                if (changed.empty())
                    ImGui::TextDisabled("Build compiles it again from the checkout first.");
                else
                    Note("Build compiles it again from the checkout first, with " + Joined(changed) +
                        " changed, so it recompiles what they touch.");
                return;
            }
            if (mCheck.mWarnings.empty())
                Line(mChoices.mOn.empty() ? "it matches the editor" : "it matches this game's options and the editor",
                    EditorTheme::Color(ThemeColor::Success));
        }
        else
        {
            Line(mCheck.mProblem, EditorTheme::Color(StatusColor::Danger));
        }
        for (const std::string &warning : mCheck.mWarnings)
            Line(warning, EditorTheme::Color(StatusColor::Warning));
    }

    void BuildSettingsPanel::DrawBuild(const BuildGame &game, const BuildJobView &build, BuildAction &action,
        const BuildAction &target)
    {
        ImGui::SeparatorText("Build");
        if (build.mRunning)
        {
            ImGui::Text("Building %s... %.0f s", game.mName.c_str(), build.mSeconds);
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
                action.mCommand = BuildCommand::Cancel;
            ImGui::SetItemTooltip("Stops the build; the previous template and build stay as they were");

            const BuildJobProgress &progress = build.mProgress;
            if (!progress.mStep.empty())
                ImGui::TextUnformatted(BuildProgressText(progress).c_str());
            if (progress.mTotal > 0)
            {
                const float done = static_cast<float>(progress.mDone) / static_cast<float>(progress.mTotal);
                ImGui::ProgressBar(std::min(done, 1.0f), ImVec2(-FLT_MIN, 0.0f));
            }
            return;
        }
        if (build.mSaving)
        {
            ImGui::TextDisabled("Saving, then building...");
            return;
        }

        const bool compile = !target.mGameBuildPreset.empty();
        const std::string blocked = !build.mBlocked.empty() ? build.mBlocked
            : !compile && !mCheck.mProblem.empty()
            ? "Pick a usable game build template, or turn on Compile the game build"
            : std::string();
        ImGui::BeginDisabled(!blocked.empty());
        if (ImGui::Button("Build"))
        {
            action = target;
            action.mCommand = BuildCommand::Build;
        }
        ImGui::EndDisabled();
        if (blocked.empty())
        {
            ImGui::SetItemTooltip(compile ? "Compiles the game build, then packages %s into the output folder"
                                          : "Packages %s into the output folder",
                game.mName.c_str());
        }
        else
        {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s", blocked.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("%s", blocked.c_str());
        }

        DrawSavePrompt(build, action, target);
        if (build.mResult != nullptr)
            DrawResult(*build.mResult, game);
    }

    void BuildSettingsPanel::DrawSavePrompt(const BuildJobView &build, BuildAction &action, const BuildAction &target)
    {
        constexpr const char *kPopupId = "Save before building?";
        if (!build.mAskSave.empty() && !ImGui::IsPopupOpen(kPopupId))
            ImGui::OpenPopup(kPopupId);

        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal(kPopupId, nullptr,
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings))
            return;

        ImGui::Text("Unsaved changes: %s.", build.mAskSave.c_str());
        ImGui::TextDisabled("The build packages what is saved on disk.");
        ImGui::Spacing();
        if (ImGui::Button("Save and Build"))
        {
            action = target;
            action.mCommand = BuildCommand::SaveThenBuild;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape) || build.mAskSave.empty())
        {
            action.mCommand = BuildCommand::DeclineSave;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    void BuildSettingsPanel::DrawResult(const BuildResult &result, const BuildGame &game)
    {
        ImGui::Spacing();
        switch (result.mOutcome)
        {
        case BuildOutcome::Succeeded:
            ImGui::TextColored(EditorTheme::Color(ThemeColor::Success), "Built in %.1f s: %zu assets, %.1f MB.",
                result.mSeconds, result.mAssets, static_cast<double>(result.mBytes) / 1e6);
            ImGui::TextWrapped("%s starts it.", CheckoutPath(result.mExecutable, game.mSourceRoot).c_str());
            break;
        case BuildOutcome::Failed:
            Line("build failed: " + result.mError, EditorTheme::Color(StatusColor::Danger));
            break;
        case BuildOutcome::Canceled:
            ImGui::TextWrapped("Build canceled. %s is as it was.", CheckoutPath(result.mOutDir, game.mSourceRoot).c_str());
            break;
        case BuildOutcome::CanceledTooLate:
            Line("too late to cancel: the new build is already in " + CheckoutPath(result.mOutDir, game.mSourceRoot),
                EditorTheme::Color(StatusColor::Warning));
            break;
        }
        for (const std::string &warning : result.mWarnings)
            Line(warning, EditorTheme::Color(StatusColor::Warning));

        if (result.mSteps.empty() || !ImGui::TreeNode("Log"))
            return;
        if (ImGui::BeginTable("##steps", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg))
        {
            for (const BuildStepResult &step : result.mSteps)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(step.mName.c_str());
                ImGui::TableNextColumn();
                if (step.mStatus == "failed")
                    ImGui::TextColored(EditorTheme::Color(StatusColor::Danger), "%s", step.mStatus.c_str());
                else if (step.mStatus == "skipped")
                    ImGui::TextDisabled("%s", step.mStatus.c_str());
                else
                    ImGui::TextUnformatted(step.mStatus.c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%.2f s", step.mSeconds);
            }
            ImGui::EndTable();
        }
        ImGui::TreePop();
    }

    BuildSettingsRequest BuildSettingsPanel::Draw(EditorPreferences &preferences, const BuildGame &game,
        const BuildJobView &build)
    {
        if (!mLoaded)
            Load(preferences, game);
        BuildSettingsRequest request;

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
        ImGui::TextDisabled(mProfile == BuildProfile::Development ? "Packages the game-debug build."
                                                                  : "Packages the game-release build.");
        ImGui::TextDisabled("Saved in project_settings.json, so the whole team builds the same way.");

        DrawOptions(game, preferences.buildCompileGameBuild);

        ImGui::SeparatorText("Game build template");
        const std::string preset(GameBuildPreset(mProfile));
        request.mSavePreferences |= ImGui::Checkbox("Compile the game build", &preferences.buildCompileGameBuild);
        ImGui::SetItemTooltip("Build compiles the game build from this checkout with the %s preset and installs it "
                              "into the template folder first. Saved for this user only",
            preset.c_str());
        const bool compile = preferences.buildCompileGameBuild;
        ImGui::TextWrapped(compile ? "Build installs the game build into this folder, then packages the game."
                                   : "Build uses a game build already in this folder, as it is.");
        ImGui::TextWrapped("Paths are saved for this user only. Empty is the default; a relative path is from the "
                           "checkout.");
        const std::string templateHint =
            CheckoutPath(DefaultGameBuildTemplate(game.mSourceRoot, mProfile), game.mSourceRoot);
        ImGui::InputTextWithHint("Template folder", templateHint.c_str(), mTemplatePath, sizeof(mTemplatePath));
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            TemplateText(preferences, mProfile) = mTemplatePath;
            request.mSavePreferences = true;
        }
        const std::filesystem::path gameBuildDir = GameBuildTemplateDir(preferences, game, mProfile);
        DrawTemplateStatus(gameBuildDir, game, compile);

        ImGui::SeparatorText("Output");
        const std::string outputHint = CheckoutPath(DefaultBuildOutput(game), game.mSourceRoot);
        ImGui::InputTextWithHint("Output folder", outputHint.c_str(), mOutputPath, sizeof(mOutputPath));
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            if (mOutputPath[0] == '\0')
                preferences.buildOutputs.erase(BuildOutputKey(game));
            else
                preferences.buildOutputs[BuildOutputKey(game)] = mOutputPath;
            request.mSavePreferences = true;
        }
        const std::filesystem::path outDir = BuildOutputDir(preferences, game);
        ImGui::TextWrapped("For this game only. %s will start the build.",
            CheckoutPath(outDir / (game.mName + ".exe"), game.mSourceRoot).c_str());

        request.mSavePreferences |= ImGui::Checkbox("Run after build", &preferences.buildRunAfter);
        ImGui::SetItemTooltip("Starts the game once a build succeeds");

        if (!mStatusMessage.empty())
            ImGui::TextColored(EditorTheme::Color(StatusColor::Danger), "%s", mStatusMessage.c_str());

        DrawBuild(game, build, request.mBuild,
            BuildAction{ BuildCommand::None, gameBuildDir, outDir, compile ? preset : std::string() });
        return request;
    }
}
