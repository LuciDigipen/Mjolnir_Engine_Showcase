/**
 * @file InspectorPanel.h
 * @author Rahul Nair
 * @brief Shows and edits the Selection's components - the editor's
 *        "Inspector" dock panel.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ComponentFields.h>
#include <core/ecs/World.h>
#include <editor/SnapToTerrain.h>
#include <editor/EditorAudio.h>
#include <editor/panels/AssetImportPanel.h>

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace mir
{
    struct ComponentOps;
    class History;
    class IAuthoredEntities;

    class InspectorPanel
    {
    public:
        
        // True when Snap to Terrain was clicked from the Transform menu.
        bool Draw(World &world, const AssetImportView &assets, History &history,
            IAuthoredEntities *authored, const SnapToTerrainAction &snap);

        // where a sound event picker's list comes from, null lists nothing
        void SetAudio(IEditorAudio *audio) { mSoundBanks.mAudio = audio; }

        // One field's value
        struct alignas(16) FieldBuffer
        {
            std::byte mBytes[64];
        };

        // A component's values copied field by field, so pasting runs each field's setter
        struct Clipboard
        {
            const ComponentOps *mOps = nullptr;
            std::vector<FieldBuffer> mValues; // one per mOps->mFields, in order
        };

        // An asset-path combo's candidates, rebuilt when its list appears
        struct AssetPathList
        {
            std::vector<std::string> mPaths;
        };

        // The sound event picker's banks, read again each time its list opens
        struct SoundBankList
        {
            IEditorAudio *mAudio = nullptr;
            std::vector<EditorSoundBank> mBanks;
            std::vector<std::array<char, kFieldStringCapacity>> mTyped; // per bank, for one whose events are unknown
        };

    private:

        void EndEditIfReleased(History &history);

        Clipboard mClipboard;
        SoundBankList mSoundBanks;
        AssetPathList mAssetPaths;
        bool mEditing = false;
        char mFilter[128] = {};
    };
} // namespace mir
