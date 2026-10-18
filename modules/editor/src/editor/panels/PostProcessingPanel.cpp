/**
 * @file PostProcessingPanel.cpp
 * @author Rahul Nair
 * @brief The "Post-Processing" window - every post effect's enable toggle
 *        and parameters, one panel (see PostEffectRegistry).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include <editor/panels/PostProcessingPanel.h>

#include <renderer/PostEffectRegistry.h>

#include <imgui.h>

namespace mir
{
    void PostProcessingPanel::Draw(PostProcessingSettings &settings, const std::filesystem::path &savePath)
    {
        if (!mDraftLoaded)
        {
            mDraftSettings = settings;
            mDraftLoaded = true;
        }

        const std::span<const PostEffectDesc> effects = PostEffectRegistry::Instance().All();
        if (effects.empty())
        {
            ImGui::TextDisabled("No post effects yet.");
        }
        else
        {
            for (const PostEffectDesc &desc : effects)
            {
                const std::string name(desc.mName);
                ImGui::PushID(name.c_str());

                PostEffectState &state = mDraftSettings.effects[name];
                ImGui::Checkbox(name.c_str(), &state.enabled);

                if (state.enabled && desc.mDrawParams != nullptr)
                {
                    ImGui::Indent();
                    desc.mDrawParams(state.params);
                    ImGui::Unindent();
                }

                ImGui::PopID();
            }
        }

        if (!mStatusMessage.empty() && mDraftSettings != mMessageSnapshot)
            mStatusMessage.clear();

        if (ImGui::Button("Save"))
        {
            if (mDraftSettings.SaveFile(savePath))
            {
                settings = mDraftSettings;
                mStatusMessage = "Saved.";
            }
            else
            {
                mStatusMessage = "Save failed - see log.";
            }
            mMessageSnapshot = mDraftSettings;
        }

        if (!mStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mStatusMessage.c_str());
        }
    }
}
