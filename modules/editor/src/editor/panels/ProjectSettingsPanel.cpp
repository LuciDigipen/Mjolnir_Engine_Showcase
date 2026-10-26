/**
 * @file ProjectSettingsPanel.cpp
 * @author Rahul Nair
 * @brief The "Project Settings" window - Rendering section, saved to disk.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/ProjectSettingsPanel.h>

#include <core/log/SetupError.h>
#include <physics_layers/PhysicsLayerSettings.h>

#include <imgui.h>

#include <cstdio>

namespace mir
{
    void ProjectSettingsPanel::DrawRendering(RenderSettings &renderSettings, RenderSettings &draftRenderSettings,
        const std::filesystem::path &savePath)
    {
        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Ray Tracing", &draftRenderSettings.rayTracing);

        if (!mRenderingStatusMessage.empty() && draftRenderSettings != mMessageSnapshot)
            mRenderingStatusMessage.clear();

        if (ImGui::Button("Save##Rendering"))
        {
            if (draftRenderSettings.SaveFile(savePath))
            {
                renderSettings = draftRenderSettings;
                mRenderingStatusMessage = "Saved.";
            }
            else
            {
                mRenderingStatusMessage = "Save failed - see log.";
            }
            mMessageSnapshot = draftRenderSettings;
        }

        if (!mRenderingStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mRenderingStatusMessage.c_str());
        }
    }

    void ProjectSettingsPanel::DrawExternalTools(const std::filesystem::path &savePath)
    {
        if (!mIdeDraftLoaded)
        {
            mDraftIdeSettings = IdeSettings::LoadFile(savePath).value_or(IdeSettings{});
            std::snprintf(mIdePathBuffer, sizeof(mIdePathBuffer), "%s", mDraftIdeSettings.path.c_str());
            std::snprintf(mIdeArgsBuffer, sizeof(mIdeArgsBuffer), "%s", mDraftIdeSettings.argsTemplate.c_str());
            mIdeDraftLoaded = true;
        }

        ImGui::SeparatorText("External Tools");
        ImGui::TextWrapped(
            "Double-clicking a .lua or .json file in the Content Browser opens the whole project folder as the "
            "workspace root in a new IDE window, with the file open. Empty IDE path auto-detects VS Code. Launch "
            "args: {project} and {file} are substituted; a non-VS Code tool may need a different template.");

        if (ImGui::InputTextWithHint("IDE path", "auto-detect (VS Code)", mIdePathBuffer, sizeof(mIdePathBuffer)))
            mDraftIdeSettings.path = mIdePathBuffer;

        if (ImGui::InputTextWithHint("Launch args", kDefaultIdeArgsTemplate, mIdeArgsBuffer, sizeof(mIdeArgsBuffer)))
            mDraftIdeSettings.argsTemplate = mIdeArgsBuffer;

        if (!mIdeStatusMessage.empty() && mDraftIdeSettings != mIdeMessageSnapshot)
            mIdeStatusMessage.clear();

        if (ImGui::Button("Detect VS Code"))
        {
            if (const std::optional<std::filesystem::path> detected = DetectDefaultIde())
            {
                mDraftIdeSettings.path = detected->string();
                std::snprintf(mIdePathBuffer, sizeof(mIdePathBuffer), "%s", mDraftIdeSettings.path.c_str());
            }
            else
                mIdeStatusMessage = "No VS Code installation found - set a path manually.";
            mIdeMessageSnapshot = mDraftIdeSettings;
        }
        ImGui::SameLine();
        if (ImGui::Button("Save##Ide"))
        {
            mIdeStatusMessage = mDraftIdeSettings.SaveFile(savePath) ? "Saved." : "Save failed - see log.";
            mIdeMessageSnapshot = mDraftIdeSettings;
        }

        if (!mIdeStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mIdeStatusMessage.c_str());
        }
    }

    void ProjectSettingsPanel::LoadLayers(const std::filesystem::path &savePath)
    {
        try
        {
            mDraftLayers = ReadPhysicsLayers(savePath);
            mLayersStatus.clear();
        }
        catch (const SetupError &)
        {
            mDraftLayers = PhysicsLayerTable{};
            mLayersStatus = "The saved layers are invalid, showing the defaults - see log.";
        }

        for (int i = 0; i < kPhysicsLayerCount; ++i)
            std::snprintf(mLayerNames[i], sizeof(mLayerNames[i]), "%s", mDraftLayers.names[i].c_str());
        mLayersLoaded = true;
    }

    void ProjectSettingsPanel::DrawPhysicsLayers(
        const std::filesystem::path &savePath, PhysicsLayerUpdate *layerUpdate, bool editing)
    {
        if (!mLayersLoaded)
            LoadLayers(savePath);

        ImGui::SeparatorText("Physics layers");
        ImGui::TextWrapped(
            "A body is on one layer and collides with the layers ticked below. Layer 0 is Default. An empty name is "
            "a free slot; clearing a name frees it without moving the others. Saved layers apply to the running "
            "world while editing.");

        if (!editing)
            ImGui::BeginDisabled();

        if (ImGui::BeginChild("##layerNames", ImVec2(0.0f, 160.0f), ImGuiChildFlags_Borders))
        {
            for (int i = 0; i < kPhysicsLayerCount; ++i)
            {
                ImGui::PushID(i);
                ImGui::TextDisabled("%2d", i);
                ImGui::SameLine();
                if (i == 0)
                    ImGui::TextUnformatted("Default");
                else if (ImGui::InputTextWithHint("##name", "unused", mLayerNames[i], sizeof(mLayerNames[i])))
                {
                    mDraftLayers.names[i] = mLayerNames[i];
                    if (mDraftLayers.names[i].empty())
                    {
                        // a freed slot goes back to colliding with everything
                        for (int other = 0; other < kPhysicsLayerCount; ++other)
                            mDraftLayers.SetCollide(i, other, true);
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        int named[kPhysicsLayerCount];
        int namedCount = 0;
        for (int i = 0; i < kPhysicsLayerCount; ++i)
        {
            if (!mDraftLayers.names[i].empty())
                named[namedCount++] = i;
        }

        ImGui::TextUnformatted("Collides with");
        if (ImGui::BeginTable("##layerMatrix", namedCount + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("");
            for (int column = 0; column < namedCount; ++column)
                ImGui::TableSetupColumn(mDraftLayers.names[named[column]].c_str());
            ImGui::TableHeadersRow();

            for (int row = 0; row < namedCount; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(mDraftLayers.names[named[row]].c_str());
                // one box per pair: the lower triangle
                for (int column = 0; column <= row; ++column)
                {
                    ImGui::TableSetColumnIndex(column + 1);
                    ImGui::PushID(row * kPhysicsLayerCount + column);
                    bool collide = mDraftLayers.Collide(named[row], named[column]);
                    if (ImGui::Checkbox("##collide", &collide))
                        mDraftLayers.SetCollide(named[row], named[column], collide);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        if (ImGui::Button("Save##Layers"))
        {
            const std::string problem = CheckPhysicsLayers(mDraftLayers);
            if (!problem.empty())
                mLayersStatus = "Not saved: " + problem + ".";
            else if (!WritePhysicsLayers(savePath, mDraftLayers))
                mLayersStatus = "Save failed - see log.";
            else
            {
                if (layerUpdate != nullptr)
                    layerUpdate->mPending = mDraftLayers;
                mLayersStatus = "Saved.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Revert##Layers"))
            LoadLayers(savePath);

        if (!editing)
            ImGui::EndDisabled();

        if (!mLayersStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mLayersStatus.c_str());
        }
    }

    void ProjectSettingsPanel::Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath,
        bool &showInputSettings, PhysicsLayerUpdate *layerUpdate, bool editing)
    {
        if (!mDraftLoaded)
        {
            mDraftRenderSettings = renderSettings;
            mDraftLoaded = true;
        }

        ImGui::SeparatorText("Input");
        ImGui::TextWrapped("Actions and bindings live in their own panel.");
        if (ImGui::Button("Open Input Settings"))
            showInputSettings = true;

        DrawRendering(renderSettings, mDraftRenderSettings, savePath);
        DrawPhysicsLayers(savePath, layerUpdate, editing);
        DrawExternalTools(savePath);
    }
}
