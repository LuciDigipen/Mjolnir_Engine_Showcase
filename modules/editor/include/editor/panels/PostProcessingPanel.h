/**
 * @file PostProcessingPanel.h
 * @author Rahul Nair
 * @brief The "Post-Processing" window - every post effect's enable toggle
 *        and parameters, one panel (see PostEffectRegistry).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <renderer/PostProcessingSettings.h>

#include <filesystem>
#include <string>

namespace mir
{
    class PostProcessingPanel
    {
    public:
        void Draw(PostProcessingSettings &settings, const std::filesystem::path &savePath);

        void OnHidden()
        {
            mDraftLoaded = false;
            mStatusMessage.clear();
        }

    private:
        std::string mStatusMessage;
        PostProcessingSettings mMessageSnapshot;

        bool mDraftLoaded = false;
        PostProcessingSettings mDraftSettings;
    };
}
