/**
 * @file TerrainLayerDocument.h
 * @author Jackey Cen
 * @brief One terrain asset's Edit Layers, their compositing order and the stack's saved state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainEditLayer.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // Edits stay in memory until Save writes the whole stack beside the recipe.
    class TerrainLayerDocument
    {
    public:
        explicit TerrainLayerDocument(std::filesystem::path layersDir);

        // A failed read leaves the document uneditable, with LoadError() set.
        void Load();

        const std::filesystem::path &Directory() const { return mDirectory; }
        const std::string &LoadError() const { return mLoadError; }
        const std::string &SaveError() const { return mSaveError; }
        bool Editable() const { return mLoadError.empty(); }

        // Differs from the stack last loaded or saved.
        bool Unsaved() const { return Editable() && mLayers != mSavedLayers; }

        const std::vector<TerrainEditLayer> &Layers() const { return mLayers; }

        // Indices into Layers(), ordered by priority then sequence so the highest is last. Disabled layers
        // keep their place, since the list shows them too.
        std::vector<std::size_t> CompositingOrder() const;
        const TerrainEditLayer *Find(uint32_t sequence) const;
        // Layers().size() when no layer has that sequence.
        std::size_t IndexOf(uint32_t sequence) const;

        // 1 + the highest sequence handed out so far; an undone add does not give its number back.
        uint32_t TakeSequence();
        std::string DefaultLayerName() const;
        // False for an empty name, an unknown layer, or a name another layer holds.
        bool CanRename(uint32_t sequence, std::string_view name) const;

        // What the commands edit; a sequence changes only when a layer is added.
        void Append(TerrainEditLayer layer);
        void Insert(std::size_t index, TerrainEditLayer layer);
        TerrainEditLayer Remove(std::size_t index);
        void SetInfo(std::size_t index, const TerrainEditLayerInfo &info);

        // Replaces the directory only when every file was written; false keeps Unsaved() and SaveError().
        bool Save();

    private:
        std::filesystem::path mDirectory;
        std::vector<TerrainEditLayer> mLayers;
        std::vector<TerrainEditLayer> mSavedLayers;
        std::string mLoadError;
        std::string mSaveError;
        uint32_t mNextSequence = 1;
    };
} // namespace mir
