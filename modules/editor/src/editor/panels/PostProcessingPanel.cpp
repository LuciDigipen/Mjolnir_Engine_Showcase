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
    void PostProcessingPanel::Draw(PostProcessingSettings &sharedSettings, const std::filesystem::path &savePath,
        const PostEffectContext &context)
    {
        if (!mDraft)
            mDraft = sharedSettings;
        PostProcessingSettings &settings = *mDraft;

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

                PostEffectState &state = settings.effects[name];
                ImGui::Checkbox(name.c_str(), &state.enabled);

                if (state.enabled && desc.mDrawParams != nullptr)
                {
                    ImGui::Indent();
                    desc.mDrawParams(state.params, context);
                    ImGui::Unindent();
                }

                ImGui::PopID();
            }
        }

        if (!mStatusMessage.empty() && settings != mSavedCopy)
            mStatusMessage.clear();

        if (ImGui::Button("Save"))
        {
            const bool saved = settings.SaveFile(savePath);
            if (saved)
                sharedSettings = settings;
            mStatusMessage = saved ? "Saved." : "Save failed - see log.";
            mSavedCopy = settings;
        }

        if (!mStatusMessage.empty())
        {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", mStatusMessage.c_str());
        }
    }
}
