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
#include <renderer/RtxgiConfig.h>

#include <imgui.h>

#include <cstdio>

namespace mir
{
    void ProjectSettingsPanel::DrawRendering(RenderSettings &renderSettings, RenderSettings &draftRenderSettings,
        const std::filesystem::path &savePath)
    {
        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Ray Tracing", &draftRenderSettings.rayTracing);

        DdgiSettings &gi = draftRenderSettings.ddgi;
        if (ImGui::TreeNode("Diffuse Global Illumination"))
        {
            ImGui::Checkbox("Enable GI", &gi.enabled);
            if (!kRtxgiCompiledIn)
                ImGui::TextWrapped("GI is unavailable in this build. Settings can still be saved.");
            else
                ImGui::TextWrapped("GI requires hardware ray tracing and a single viewport. Ambient lighting is used outside the volume.");
            if (!draftRenderSettings.rayTracing)
                ImGui::TextWrapped("GI requires ray tracing. Raster rendering uses its existing lighting.");
            ImGui::InputScalarN("Volume center (world meters)", ImGuiDataType_Double, gi.origin.data(), 3,
                nullptr, nullptr, "%.6f");
            ImGui::InputInt3("Probe counts (XYZ)", gi.probeCounts.data());
            ImGui::InputFloat("Probe spacing (meters)", &gi.probeSpacing);
            constexpr int rayCounts[]{ 64, 128, 256, 512, 1024 };
            constexpr const char *rayLabels[]{ "64", "128", "256", "512", "1024" };
            int rayChoice = 0;
            for (int i = 0; i < 5; ++i)
                if (gi.raysPerProbe == rayCounts[i])
                    rayChoice = i;
            if (ImGui::Combo("Rays per probe", &rayChoice, rayLabels, 5))
                gi.raysPerProbe = rayCounts[rayChoice];
            ImGui::InputInt("Probes updated per frame", &gi.probesPerFrame);
            ImGui::SliderFloat("GI intensity", &gi.intensity, 0.0f, 10.0f);
            if (gi.ValidationError() == nullptr)
            {
                ImGui::Text("%llu probes, %llu rays/frame, %.1f MiB estimated",
                    static_cast<unsigned long long>(gi.ProbeCount()),
                    static_cast<unsigned long long>(uint64_t(gi.probesPerFrame) * gi.raysPerProbe),
                    double(gi.EstimatedBytes()) / (1024.0 * 1024.0));
            }
            ImGui::TreePop();
        }

        SsgiSettings &ssgi = draftRenderSettings.ssgi;
        if (ImGui::TreeNode("Screen Space Global Illumination"))
        {
            ImGui::Checkbox("Enable SSGI", &ssgi.enabled);
            ImGui::TextWrapped("Uses visible opaque surfaces for indirect lighting; unresolved samples fall back to ambient light.");
            if (draftRenderSettings.rayTracing)
                ImGui::TextWrapped("SSGI settings apply to raster rendering. Ray tracing uses its own GI settings.");
            constexpr const char *qualities[]{ "Low", "Medium", "High" };
            int quality = static_cast<int>(ssgi.quality);
            if (ImGui::Combo("Quality", &quality, qualities, 3))
                ssgi.quality = static_cast<SsgiQuality>(quality);
            ImGui::Text("%u rays/pixel, up to %u steps/ray", ssgi.RaysPerPixel(), ssgi.MaxRaySteps());
            ImGui::InputFloat("Maximum trace distance (meters)", &ssgi.maxTraceDistance);
            ImGui::InputFloat("Surface thickness (meters)", &ssgi.thickness, 0.0f, 0.0f, "%.3f");
            ImGui::SliderFloat("Indirect intensity", &ssgi.intensity, 0.0f, 10.0f);
            ImGui::Checkbox("Half resolution", &ssgi.halfResolution);
            ImGui::TreePop();
        }

        const char *giError = gi.ValidationError();
        const char *ssgiError = ssgi.ValidationError();
        if (giError)
            ImGui::TextWrapped("%s", giError);
        if (ssgiError)
            ImGui::TextWrapped("%s", ssgiError);

        if (!mRenderingStatusMessage.empty() && draftRenderSettings != mMessageSnapshot)
            mRenderingStatusMessage.clear();

        ImGui::BeginDisabled(giError != nullptr || ssgiError != nullptr);
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
        ImGui::EndDisabled();

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
        ImGui::TextWrapped("A body is on one layer and collides with the layers ticked below. Layer 0 is Default.");

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
