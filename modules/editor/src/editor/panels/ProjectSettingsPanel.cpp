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

#include <cmath>
#include <cstdio>

namespace mir
{
    namespace
    {
        // the volume as a percent and the mute beside it, true when either changed
        bool DrawAudioChannel(const char *name, EditorAudioChannel &channel)
        {
            ImGui::PushID(name);
            bool changed = false;
            int percent = static_cast<int>(std::lround(channel.mVolume * 100.0f));
            if (ImGui::SliderInt(name, &percent, 0, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp))
            {
                channel.mVolume = static_cast<float>(percent) / 100.0f;
                changed = true;
            }
            ImGui::SameLine();
            changed |= ImGui::Checkbox("Muted", &channel.mMuted);
            ImGui::PopID();
            return changed;
        }
    }

    void ProjectSettingsPanel::DrawRendering(RenderSettings &renderSettings, const std::filesystem::path &savePath)
    {
        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Ray Tracing", &renderSettings.rayTracing);

        // Typing can pass through a value DdgiSettings rejects, so the widgets edit a copy while one is active
        // and the shared value changes only when the copy is valid.
        DdgiSettings gi = mDdgiEdit ? *mDdgiEdit : renderSettings.ddgi;
        bool giEdited = false;
        bool giActive = false;
        const auto trackGi = [&](bool changed) {
            giEdited |= changed;
            giActive |= ImGui::IsItemActive();
        };
        if (ImGui::TreeNode("Diffuse Global Illumination"))
        {
            trackGi(ImGui::Checkbox("Enable GI", &gi.enabled));
            if (!kRtxgiCompiledIn)
                ImGui::TextWrapped("GI is unavailable in this build. Settings can still be saved.");
            else
                ImGui::TextWrapped("GI is available in this build.");
            if (!renderSettings.rayTracing)
                ImGui::TextWrapped("GI requires ray tracing. Raster rendering uses its existing lighting.");
            trackGi(ImGui::InputScalarN("Volume center (world meters)", ImGuiDataType_Double, gi.origin.data(), 3,
                nullptr, nullptr, "%.6f"));
            trackGi(ImGui::InputInt3("Probe counts (XYZ)", gi.probeCounts.data()));
            trackGi(ImGui::InputFloat("Probe spacing (meters)", &gi.probeSpacing));
            constexpr int rayCounts[]{ 64, 128, 256, 512, 1024 };
            constexpr const char *rayLabels[]{ "64", "128", "256", "512", "1024" };
            int rayChoice = 0;
            for (int i = 0; i < 5; ++i)
                if (gi.raysPerProbe == rayCounts[i])
                    rayChoice = i;
            const bool raysChanged = ImGui::Combo("Rays per probe", &rayChoice, rayLabels, 5);
            if (raysChanged)
                gi.raysPerProbe = rayCounts[rayChoice];
            trackGi(raysChanged);
            trackGi(ImGui::InputInt("Probes updated per frame", &gi.probesPerFrame));
            trackGi(ImGui::SliderFloat("GI intensity", &gi.intensity, 0.0f, 10.0f));
            if (gi.ValidationError() == nullptr)
            {
                ImGui::Text("%llu probes, %llu rays/frame, %.1f MiB estimated",
                    static_cast<unsigned long long>(gi.ProbeCount()),
                    static_cast<unsigned long long>(uint64_t(gi.probesPerFrame) * gi.raysPerProbe),
                    double(gi.EstimatedBytes()) / (1024.0 * 1024.0));
            }
            ImGui::TreePop();
        }
        if (giEdited)
        {
            mDdgiEdit = gi;
            if (gi.ValidationError() == nullptr)
                renderSettings.ddgi = gi;
        }
        else if (!giActive)
            mDdgiEdit.reset();

        // Typing can pass through a value SsgiSettings rejects, so the widgets edit a copy while one is active
        // and the shared value changes only when the copy is valid.
        SsgiSettings ssgi = mSsgiEdit ? *mSsgiEdit : renderSettings.ssgi;
        bool ssgiEdited = false;
        bool ssgiActive = false;
        const auto track = [&](bool changed) {
            ssgiEdited |= changed;
            ssgiActive |= ImGui::IsItemActive();
        };
        if (ImGui::TreeNode("Screen Space Global Illumination"))
        {
            track(ImGui::Checkbox("Enable SSGI", &ssgi.enabled));
            if (renderSettings.rayTracing)
                ImGui::TextWrapped("SSGI settings apply to raster rendering. Ray tracing uses its own GI settings.");
            constexpr const char *qualities[]{ "Low", "Medium", "High" };
            int quality = static_cast<int>(ssgi.quality);
            const bool qualityChanged = ImGui::Combo("Quality", &quality, qualities, 3);
            if (qualityChanged)
                ssgi.quality = static_cast<SsgiQuality>(quality);
            track(qualityChanged);
            ImGui::Text("%u rays/pixel, up to %u steps/ray", ssgi.RaysPerPixel(), ssgi.MaxRaySteps());
            track(ImGui::InputFloat("Maximum trace distance (meters)", &ssgi.maxTraceDistance));
            track(ImGui::InputFloat("Surface thickness (meters)", &ssgi.thickness, 0.0f, 0.0f, "%.3f"));
            track(ImGui::SliderFloat("Indirect intensity", &ssgi.intensity, 0.0f, 10.0f));
            track(ImGui::Checkbox("Half resolution", &ssgi.halfResolution));
            ImGui::TreePop();
        }
        if (ssgiEdited)
        {
            mSsgiEdit = ssgi;
            if (ssgi.ValidationError() == nullptr)
                renderSettings.ssgi = ssgi;
        }
        else if (!ssgiActive)
            mSsgiEdit.reset();

        const char *giError = gi.ValidationError();
        const char *ssgiError = ssgi.ValidationError();
        if (giError)
            ImGui::TextWrapped("%s", giError);
        if (ssgiError)
            ImGui::TextWrapped("%s", ssgiError);

        if (!mRenderingStatusMessage.empty() && renderSettings != mRenderingSavedCopy)
            mRenderingStatusMessage.clear();

        ImGui::BeginDisabled(giError != nullptr || ssgiError != nullptr);
        if (ImGui::Button("Save##Rendering"))
        {
            mRenderingStatusMessage = renderSettings.SaveFile(savePath) ? "Saved." : "Save failed - see log.";
            mRenderingSavedCopy = renderSettings;
        }
        ImGui::EndDisabled();

        if (!mRenderingStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mRenderingStatusMessage.c_str());
        }
    }

    void ProjectSettingsPanel::DrawAudio(IEditorAudio &audio, bool editing)
    {
        ImGui::SeparatorText("Audio");
        ImGui::TextWrapped("What a player starts with, until they change it in the pause menu.");

        ImGui::BeginDisabled(!editing);
        ImGui::PushID("defaults");
        EditorAudioLevels defaults = audio.Defaults();
        bool defaultsChanged = DrawAudioChannel("Music", defaults.mMusic);
        defaultsChanged |= DrawAudioChannel("SFX", defaults.mSfx);
        ImGui::PopID();
        if (defaultsChanged)
            audio.SetDefaults(defaults);

        if (!mAudioStatus.empty() && audio.Defaults() != mAudioSavedCopy)
            mAudioStatus.clear();

        // Heard by a player with no settings file of their own, through the apply path, not by this session.
        if (ImGui::Button("Save##Audio"))
        {
            mAudioStatus = audio.SaveDefaults(audio.Defaults()) ? "Saved." : "Save failed - see log.";
            mAudioSavedCopy = audio.Defaults();
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload Saved Audio Defaults##Audio"))
        {
            std::string problem;
            mAudioStatus = audio.ReloadDefaults(problem) ? "Reloaded." : "Not reloaded: " + problem + ".";
            mAudioSavedCopy = audio.Defaults();
        }
        ImGui::EndDisabled();

        if (!mAudioStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mAudioStatus.c_str());
        }

        if (editing)
            return;

        ImGui::Spacing();
        ImGui::TextUnformatted("While playing");
        ImGui::TextWrapped("Heard at once and put back on Stop; the defaults above stay as saved.");
        EditorAudioLevels live = audio.Live();
        ImGui::PushID("live");
        bool changed = DrawAudioChannel("Music", live.mMusic);
        changed |= DrawAudioChannel("SFX", live.mSfx);
        ImGui::PopID();
        if (changed)
            audio.SetLive(live);
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

    // Into a temporary first, so a broken file leaves the project's table as it is.
    void ProjectSettingsPanel::ReloadLayers(const std::filesystem::path &savePath, PhysicsLayerTable &sharedLayers)
    {
        try
        {
            const PhysicsLayerTable saved = ReadPhysicsLayers(savePath);
            const std::string problem = CheckPhysicsLayers(saved);
            if (!problem.empty())
            {
                mLayersStatus = "Not reloaded: " + problem + ".";
                return;
            }
            sharedLayers = saved;
            mLayersStatus = "Reloaded.";
        }
        catch (const SetupError &error)
        {
            mLayersStatus = std::string("Not reloaded: ") + std::string(error.Cause()) + ".";
        }
    }

    void ProjectSettingsPanel::DrawPhysicsLayers(
        const std::filesystem::path &savePath, PhysicsLayerTable *sharedLayers, bool editing)
    {
        ImGui::SeparatorText("Physics layers");
        if (sharedLayers == nullptr)
        {
            ImGui::TextDisabled("No physics layers in this scene.");
            return;
        }
        PhysicsLayerTable &layers = *sharedLayers;
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
                else
                {
                    const bool editingSlot = mLayerNameEdit && mLayerNameEdit->slot == i;
                    char name[64]{};
                    std::snprintf(name, sizeof(name), "%s",
                                  (editingSlot ? mLayerNameEdit->text : layers.names[i]).c_str());
                    if (ImGui::InputTextWithHint("##name", "unused", name, sizeof(name)))
                    {
                        // A copy takes the name first, so a rejected table never reaches the shared one.
                        PhysicsLayerTable candidate = layers;
                        candidate.names[i] = name;
                        if (candidate.names[i].empty())
                        {
                            // a freed slot goes back to colliding with everything
                            for (int other = 0; other < kPhysicsLayerCount; ++other)
                                candidate.SetCollide(i, other, true);
                        }
                        mLayerNameEdit = LayerNameEdit{ i, name };
                        mLayerNameProblem = CheckPhysicsLayers(candidate);
                        if (mLayerNameProblem.empty())
                            layers = candidate;
                    }
                    if (!ImGui::IsItemActive() && mLayerNameEdit && mLayerNameEdit->slot == i)
                    {
                        mLayerNameEdit.reset();
                        mLayerNameProblem.clear();
                    }
                    if (mLayerNameEdit && mLayerNameEdit->slot == i && !mLayerNameProblem.empty())
                        ImGui::TextWrapped("Not applied: %s.", mLayerNameProblem.c_str());
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();

        int named[kPhysicsLayerCount];
        int namedCount = 0;
        for (int i = 0; i < kPhysicsLayerCount; ++i)
        {
            if (!layers.names[i].empty())
                named[namedCount++] = i;
        }

        ImGui::TextUnformatted("Collides with");
        if (ImGui::BeginTable("##layerMatrix", namedCount + 1, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit))
        {
            ImGui::TableSetupColumn("");
            for (int column = 0; column < namedCount; ++column)
                ImGui::TableSetupColumn(layers.names[named[column]].c_str());
            ImGui::TableHeadersRow();

            for (int row = 0; row < namedCount; ++row)
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(layers.names[named[row]].c_str());
                // one box per pair: the lower triangle
                for (int column = 0; column <= row; ++column)
                {
                    ImGui::TableSetColumnIndex(column + 1);
                    ImGui::PushID(row * kPhysicsLayerCount + column);
                    bool collide = layers.Collide(named[row], named[column]);
                    if (ImGui::Checkbox("##collide", &collide))
                        layers.SetCollide(named[row], named[column], collide);
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();
        }

        if (ImGui::Button("Save##Layers"))
        {
            const std::string problem = CheckPhysicsLayers(layers);
            if (!problem.empty())
                mLayersStatus = "Not saved: " + problem + ".";
            else if (!WritePhysicsLayers(savePath, layers))
                mLayersStatus = "Save failed - see log.";
            else
                mLayersStatus = "Saved.";
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload Saved Physics Layers##Layers"))
            ReloadLayers(savePath, layers);

        if (!editing)
            ImGui::EndDisabled();

        if (!mLayersStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mLayersStatus.c_str());
        }
    }

    void ProjectSettingsPanel::Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath,
        bool &showInputSettings, PhysicsLayerTable *sharedLayers, IEditorAudio *audio, bool editing)
    {
        ImGui::SeparatorText("Input");
        ImGui::TextWrapped("Actions and bindings live in their own panel.");
        if (ImGui::Button("Open Input Settings"))
            showInputSettings = true;

        DrawRendering(renderSettings, savePath);
        DrawPhysicsLayers(savePath, sharedLayers, editing);
        if (audio != nullptr)
            DrawAudio(*audio, editing);
        DrawExternalTools(savePath);
    }
}
