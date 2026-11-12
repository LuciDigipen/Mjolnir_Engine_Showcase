/**
 * @file TerrainLayerDocument.cpp
 * @author Jackey Cen
 * @brief One terrain asset's Edit Layers, their compositing order and the stack's saved state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainLayerDocument.h>

#include <core/fs/FileIO.h>
#include <core/log/Log.h>
#include <terrain/TerrainEditStore.h>

#include <algorithm>
#include <format>
#include <limits>
#include <numeric>
#include <system_error>
#include <utility>

namespace mir
{
    namespace
    {
        std::filesystem::path Sibling(const std::filesystem::path &directory, std::string_view suffix)
        {
            return directory.parent_path() / (directory.filename().string() + std::string(suffix));
        }
    } // namespace

    TerrainLayerDocument::TerrainLayerDocument(std::filesystem::path layersDir)
        : mDirectory(std::move(layersDir))
    {
    }

    void TerrainLayerDocument::Load()
    {
        const TerrainResult<std::vector<TerrainEditLayer>> loaded = LoadTerrainEditStack(mDirectory);
        if (!loaded)
        {
            mLoadError = loaded.error.message;
            mLayers.clear();
            mSavedLayers.clear();
            return;
        }

        mLoadError.clear();
        mLayers = *loaded.value;
        mSavedLayers = mLayers;

        std::size_t highest = 0;
        for (const TerrainEditLayer &layer : mLayers)
            highest = std::max(highest, static_cast<std::size_t>(layer.info.sequence));
        mNextSequence = highest == std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                                       : static_cast<uint32_t>(highest) + 1;
    }

    std::vector<std::size_t> TerrainLayerDocument::CompositingOrder() const
    {
        std::vector<std::size_t> order(mLayers.size());
        std::iota(order.begin(), order.end(), std::size_t{ 0 });
        std::ranges::stable_sort(order, [this](std::size_t left, std::size_t right) {
            const TerrainEditLayerInfo &a = mLayers[left].info;
            const TerrainEditLayerInfo &b = mLayers[right].info;
            return a.priority != b.priority ? a.priority < b.priority : a.sequence < b.sequence;
        });
        return order;
    }

    const TerrainEditLayer *TerrainLayerDocument::Find(uint32_t sequence) const
    {
        const std::size_t index = IndexOf(sequence);
        return index < mLayers.size() ? &mLayers[index] : nullptr;
    }

    std::size_t TerrainLayerDocument::IndexOf(uint32_t sequence) const
    {
        const auto found = std::ranges::find(mLayers, sequence, [](const TerrainEditLayer &layer) {
            return layer.info.sequence;
        });
        return found == mLayers.end() ? mLayers.size() : static_cast<std::size_t>(found - mLayers.begin());
    }

    uint32_t TerrainLayerDocument::TakeSequence()
    {
        const uint32_t sequence = mNextSequence;
        if (mNextSequence != std::numeric_limits<uint32_t>::max())
            ++mNextSequence;
        return sequence;
    }

    std::string TerrainLayerDocument::DefaultLayerName() const
    {
        for (std::size_t number = 1;; ++number)
        {
            std::string name = std::format("Layer {}", number);
            const bool used = std::ranges::any_of(mLayers, [&name](const TerrainEditLayer &layer) {
                return layer.info.name == name;
            });
            if (!used)
                return name;
        }
    }

    bool TerrainLayerDocument::CanRename(uint32_t sequence, std::string_view name) const
    {
        if (name.empty() || Find(sequence) == nullptr)
            return false;
        return std::ranges::none_of(mLayers, [sequence, name](const TerrainEditLayer &layer) {
            return layer.info.sequence != sequence && layer.info.name == name;
        });
    }

    void TerrainLayerDocument::Append(TerrainEditLayer layer)
    {
        Insert(mLayers.size(), std::move(layer));
    }

    void TerrainLayerDocument::Insert(std::size_t index, TerrainEditLayer layer)
    {
        if (layer.info.sequence >= mNextSequence)
            mNextSequence = layer.info.sequence == std::numeric_limits<uint32_t>::max()
                                ? std::numeric_limits<uint32_t>::max()
                                : layer.info.sequence + 1;
        mLayers.insert(mLayers.begin() + static_cast<std::ptrdiff_t>(std::min(index, mLayers.size())),
            std::move(layer));
    }

    TerrainEditLayer TerrainLayerDocument::Remove(std::size_t index)
    {
        TerrainEditLayer removed = std::move(mLayers[index]);
        mLayers.erase(mLayers.begin() + static_cast<std::ptrdiff_t>(index));
        return removed;
    }

    void TerrainLayerDocument::SetInfo(std::size_t index, const TerrainEditLayerInfo &info)
    {
        mLayers[index].info = info;
    }

    bool TerrainLayerDocument::Save()
    {
        mSaveError.clear();
        const auto fail = [this](std::string message) {
            mSaveError = std::move(message);
            MIR_LOG_ERROR("Terrain Edit Layers: {}", mSaveError);
            return false;
        };

        if (!Editable())
            return fail(std::format("{}: the Edit Layers did not load", mDirectory.generic_string()));

        const std::filesystem::path saving = Sibling(mDirectory, ".saving");
        const std::filesystem::path aside = Sibling(mDirectory, ".old");
        std::error_code ec;
        std::filesystem::remove_all(saving, ec);
        if (ec)
            return fail(std::format("{}: cannot remove the interrupted save ({})", saving.generic_string(),
                ec.message()));

        std::filesystem::remove_all(aside, ec);
        if (ec)
            return fail(std::format("{}: cannot remove the replaced Edit Layers ({})", aside.generic_string(),
                ec.message()));

        const auto removeSaving = [&saving]() {
            std::error_code ignored;
            std::filesystem::remove_all(saving, ignored);
        };

        if (mLayers.empty())
        {
            std::filesystem::remove_all(mDirectory, ec);
            if (ec)
                return fail(std::format("{}: cannot remove the Edit Layer directory ({})",
                    mDirectory.generic_string(), ec.message()));
            mSavedLayers.clear();
            return true;
        }

        const TerrainResult<std::vector<TerrainEditStackFile>> planned = PlanTerrainEditStackWrite(mLayers);
        if (!planned)
            return fail(planned.error.message);

        std::filesystem::create_directories(saving, ec);
        if (ec)
            return fail(std::format("{}: cannot create the new Edit Layer directory ({})",
                saving.generic_string(), ec.message()));

        for (const TerrainEditStackFile &file : *planned.value)
        {
            const std::filesystem::path path = saving / file.path;
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec)
            {
                removeSaving();
                return fail(std::format("{}: cannot create the Edit Layer directory ({})",
                    path.parent_path().generic_string(), ec.message()));
            }
            if (!WriteBytes(path, file.bytes))
            {
                removeSaving();
                return fail(std::format("{}: cannot write the Edit Layer file", path.generic_string()));
            }
        }

        if (std::filesystem::exists(mDirectory, ec))
        {
            std::filesystem::rename(mDirectory, aside, ec);
            if (ec)
            {
                removeSaving();
                return fail(std::format("{}: cannot move the current Edit Layers aside ({})",
                    mDirectory.generic_string(), ec.message()));
            }
        }

        std::filesystem::rename(saving, mDirectory, ec);
        if (ec)
        {
            std::error_code restore;
            if (std::filesystem::exists(aside, restore))
                std::filesystem::rename(aside, mDirectory, restore);
            removeSaving();
            return fail(std::format("{}: cannot move the new Edit Layers in ({})", mDirectory.generic_string(),
                ec.message()));
        }

        std::filesystem::remove_all(aside, ec);
        mSavedLayers = mLayers;
        return true;
    }
} // namespace mir
