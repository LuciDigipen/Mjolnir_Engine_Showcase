/**
 * @file ProjectSettingsPanel.h
 * @author Rahul Nair
 * @brief The "Project Settings" window - Rendering section, saved to disk.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/EditorAudio.h>
#include <editor/IdeSettings.h>
#include <physics_layers/PhysicsLayers.h>
#include <renderer/RenderSettings.h>

#include <filesystem>
#include <optional>
#include <string>
#include <utility>

namespace mir
{
    class VulkanRenderer;
    class ProjectSettingsPanel
    {
    public:

        void Draw(RenderSettings &renderSettings, const std::filesystem::path &savePath, bool &showInputSettings,
            PhysicsLayerTable *sharedLayers, IEditorAudio *audio, bool editing, const VulkanRenderer *renderer = nullptr);

        // The editor forwards these to the renderer; the panel only reads it.
        bool ShowGiVolume() const { return mShowGiVolume; }
        bool TakeGiSceneBoundsRequest() { return std::exchange(mGiSceneBoundsRequested, false); }

        void OnHidden()
        {
            mRenderingDraft.reset();
            mLayersDraft.reset();
            mAudioDraft.reset();
            mLayersStatus.clear();
            mRenderingStatusMessage.clear();
            mSsgiEdit.reset();
            mDdgiEdit.reset();
            mLayerNameEdit.reset();
            mLayerNameProblem.clear();
            mIdeDraftLoaded = false;
            mIdeStatusMessage.clear();
            mAudioStatus.clear();
        }

    private:
        void DrawRendering(RenderSettings &renderSettings, const std::filesystem::path &savePath);
        void DrawAudio(IEditorAudio &audio, bool editing);
        void DrawExternalTools(const std::filesystem::path &savePath);
        void DrawPhysicsLayers(const std::filesystem::path &savePath, PhysicsLayerTable *sharedLayers, bool editing);
        bool ReloadLayers(const std::filesystem::path &savePath, PhysicsLayerTable &sharedLayers);

        std::optional<RenderSettings> mRenderingDraft;
        std::optional<PhysicsLayerTable> mLayersDraft;
        std::optional<EditorAudioLevels> mAudioDraft;
        std::string mRenderingStatusMessage;
        // What Save wrote, so the message clears on the next edit. Nothing else reads it.
        RenderSettings mRenderingSavedCopy;
        // The SSGI value being typed, kept only while one of its widgets is active so an invalid value never
        // reaches the draft.
        std::optional<SsgiSettings> mSsgiEdit;
        // The same for the DDGI value, whose widgets can also pass through a value it rejects.
        std::optional<DdgiSettings> mDdgiEdit;
        const VulkanRenderer *mRenderer = nullptr;
        float mGiPadding = 4.0f;
        std::string mGiFitStatus;
        bool mShowGiVolume = false;
        bool mGiSceneBoundsRequested = false;

        // The layer name being typed, by slot, kept while its field is active. A name another layer already
        // uses reaches the draft only once it is valid.
        struct LayerNameEdit
        {
            int slot = 0;
            std::string text;
        };
        std::optional<LayerNameEdit> mLayerNameEdit;
        std::string mLayerNameProblem;

        std::string mLayersStatus;

        std::string mAudioStatus;
        EditorAudioLevels mAudioSavedCopy;

        std::string mIdeStatusMessage;
        IdeSettings mIdeMessageSnapshot;
        bool mIdeDraftLoaded = false;
        IdeSettings mDraftIdeSettings;
        char mIdePathBuffer[512]{};
        char mIdeArgsBuffer[256]{};
    };
}
