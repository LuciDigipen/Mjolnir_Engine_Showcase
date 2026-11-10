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
#include <renderer/VulkanRenderer.h>

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

    void ProjectSettingsPanel::DrawRendering(RenderSettings &sharedSettings, const std::filesystem::path &savePath)
    {
        if (!mRenderingDraft)
            mRenderingDraft = sharedSettings;
        RenderSettings &renderSettings = *mRenderingDraft;

        ImGui::SeparatorText("Rendering");

        ImGui::Checkbox("Ray Tracing", &renderSettings.rayTracing);

        // Typing can pass through a value DdgiSettings rejects, so the widgets edit a copy while one is active
        // and the draft changes only when the copy is valid.
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
                ImGui::TextWrapped("RTXGI is compiled into this build.");
            if (mRenderer) ImGui::TextWrapped("Runtime: %s", mRenderer->GiStatus().c_str());
            if (!renderSettings.rayTracing)
                ImGui::TextWrapped("GI requires ray tracing. Raster rendering uses its existing lighting.");
            trackGi(ImGui::InputScalarN("Volume center (world meters)", ImGuiDataType_Double, gi.origin.data(), 3,
                nullptr, nullptr, "%.6f"));
            trackGi(ImGui::InputInt3("Probe counts (XYZ)", gi.probeCounts.data()));
            trackGi(ImGui::InputFloat("Probe spacing (meters)", &gi.probeSpacing));
            ImGui::InputFloat("Bounds padding (meters)", &mGiPadding);
            if (mRenderer)
            {
                // Keeps resident scene bounds fresh only while this section is drawn.
                mGiSceneBoundsRequested = true;
                const auto bounds = mRenderer->GetGiSceneBounds();
                ImGui::BeginDisabled(!bounds);
                if (ImGui::Button("Fit to resident scene bounds"))
                {
                    const char *error = gi.FitBounds(bounds->minimum, bounds->maximum, mGiPadding);
                    // A fit reaches the draft only when valid and applies when Save succeeds.
                    trackGi(error == nullptr);
                    mGiFitStatus = error ? error : "Fitted. Save to write it to the project file.";
                    if (error)
                    {
                        auto alternative = gi;
                        for (int attempt = 0; attempt < 16; ++attempt)
                        {
                            alternative.probeSpacing *= 1.25f;
                            if (!alternative.FitBounds(bounds->minimum, bounds->maximum, mGiPadding))
                            {
                                mGiFitStatus += " Try spacing " + std::to_string(alternative.probeSpacing) + " m.";
                                break;
                            }
                        }
                    }
                }
                ImGui::EndDisabled();
                ImGui::Checkbox("Show saved GI volume and probe grid", &mShowGiVolume);
                ImGui::TextWrapped("GPU probe state: green active, gray inactive, yellow awaiting readback. Relocation and state arrive a few frames later.");
                ImGui::TextWrapped("Fits loaded mesh bounds, including model transforms. Allow padding for animation and streamed geometry.");
            }
            if (!mGiFitStatus.empty()) ImGui::TextWrapped("%s", mGiFitStatus.c_str());
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
                const auto extent = gi.Extent();
                ImGui::Text("Coverage: %.1f x %.1f x %.1f m", extent[0], extent[1], extent[2]);
                ImGui::Text("Full sweep: %llu frames", static_cast<unsigned long long>(
                    (gi.ProbeCount() + gi.probesPerFrame - 1) / gi.probesPerFrame));
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
        // and the draft changes only when the copy is valid.
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
            const bool saved = renderSettings.SaveFile(savePath);
            if (saved)
                sharedSettings = renderSettings;
            mRenderingStatusMessage = saved ? "Saved." : "Save failed - see log.";
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
        if (!mAudioDraft)
            mAudioDraft = audio.Defaults();
        EditorAudioLevels &defaults = *mAudioDraft;
        DrawAudioChannel("Music", defaults.mMusic);
        DrawAudioChannel("SFX", defaults.mSfx);
        ImGui::PopID();

        if (!mAudioStatus.empty() && defaults != mAudioSavedCopy)
            mAudioStatus.clear();

        // Heard by a player with no settings file of their own, through the apply path, not by this session.
        if (ImGui::Button("Save##Audio"))
        {
            mAudioStatus = audio.SaveDefaults(defaults) ? "Saved." : "Save failed - see log.";
            mAudioSavedCopy = defaults;
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload Saved Audio Defaults##Audio"))
        {
            std::string problem;
            const bool reloaded = audio.ReloadDefaults(problem);
            mAudioStatus = reloaded ? "Reloaded." : "Not reloaded: " + problem + ".";
            if (reloaded)
                defaults = audio.Defaults();
            mAudioSavedCopy = defaults;
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
    bool ProjectSettingsPanel::ReloadLayers(const std::filesystem::path &savePath, PhysicsLayerTable &sharedLayers)
    {
        try
        {
            const PhysicsLayerTable saved = ReadPhysicsLayers(savePath);
            const std::string problem = CheckPhysicsLayers(saved);
            if (!problem.empty())
            {
                mLayersStatus = "Not reloaded: " + problem + ".";
                return false;
            }
            sharedLayers = saved;
            mLayersStatus = "Reloaded.";
            return true;
        }
        catch (const SetupError &error)
        {
            mLayersStatus = std::string("Not reloaded: ") + std::string(error.Cause()) + ".";
            return false;
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
        if (!mLayersDraft)
            mLayersDraft = *sharedLayers;
        PhysicsLayerTable &layers = *mLayersDraft;
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
                        // A copy takes the name first, so a rejected table never reaches the draft.
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
            {
                *sharedLayers = layers;
                mLayersStatus = "Saved.";
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reload Saved Physics Layers##Layers"))
        {
            if (ReloadLayers(savePath, *sharedLayers))
                layers = *sharedLayers;
        }

        if (!editing)
            ImGui::EndDisabled();

        if (!mLayersStatus.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mLayersStatus.c_str());
        }
    }

    void ProjectSettingsPanel::Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath,
        bool &showInputSettings, PhysicsLayerTable *sharedLayers, IEditorAudio *audio, bool editing, const VulkanRenderer *renderer)
    {
        mRenderer = renderer;

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
