/**
 * @file ProjectSettingsPanel.h
 * @author Rahul Nair
 * @brief The "Project Settings" window - Rendering section, saved to disk.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/IdeSettings.h>
#include <physics_layers/PhysicsLayers.h>
#include <renderer/RenderSettings.h>

#include <filesystem>
#include <string>

namespace mir
{
    class ProjectSettingsPanel
    {
    public:
        // layerUpdate: where a saved layer table is left for physics, null when there is none; editing: only in Edit
        void Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath, bool &showInputSettings,
            PhysicsLayerUpdate *layerUpdate, bool editing);

        void OnHidden()
        {
            mLayersLoaded = false;
            mLayersStatus.clear();
            mDraftLoaded = false;
            mRenderingStatusMessage.clear();
            mIdeDraftLoaded = false;
            mIdeStatusMessage.clear();
        }

    private:
        void DrawRendering(RenderSettings &renderSettings, RenderSettings &draftRenderSettings,
            const std::filesystem::path &savePath);
        void DrawExternalTools(const std::filesystem::path &savePath);
        void DrawPhysicsLayers(const std::filesystem::path &savePath, PhysicsLayerUpdate *layerUpdate, bool editing);
        void LoadLayers(const std::filesystem::path &savePath);

        std::string mRenderingStatusMessage;
        RenderSettings mMessageSnapshot;

        bool mDraftLoaded = false;
        RenderSettings mDraftRenderSettings;

        bool mLayersLoaded = false;
        PhysicsLayerTable mDraftLayers;
        char mLayerNames[kPhysicsLayerCount][64]{};
        std::string mLayersStatus;

        std::string mIdeStatusMessage;
        IdeSettings mIdeMessageSnapshot;
        bool mIdeDraftLoaded = false;
        IdeSettings mDraftIdeSettings;
        char mIdePathBuffer[512]{};
        char mIdeArgsBuffer[256]{};
    };
}
