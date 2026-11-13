/**
 * @file TerrainLayerCommands.cpp
 * @author Jackey Cen
 * @brief Undoable add, delete, rename, priority, move and enable changes to an Edit Layer stack.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <terrain_editor/TerrainLayerCommands.h>

#include <terrain_editor/TerrainLayerDocument.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace mir
{
    AddTerrainLayerCommand::AddTerrainLayerCommand(TerrainLayerDocument &document, TerrainEditBlend blend)
        : TerrainLayerCommand(document), mBlend(blend)
    {
    }

    bool AddTerrainLayerCommand::Do(CommandContext &)
    {
        if (!mCreated)
        {
            int32_t highest = std::numeric_limits<int32_t>::min();
            for (const TerrainEditLayer &layer : mDocument.Layers())
                highest = std::max(highest, layer.info.priority);

            int32_t priority = 0;
            if (!mDocument.Layers().empty())
                priority = highest == std::numeric_limits<int32_t>::max() ? highest : highest + 1;

            mLayer.info = TerrainEditLayerInfo{ mDocument.DefaultLayerName(), priority, mDocument.TakeSequence(), true,
                mBlend };
            mCreated = true;
        }

        mDocument.Append(mLayer);
        return true;
    }

    bool AddTerrainLayerCommand::Undo(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mLayer.info.sequence);
        if (!mCreated || index >= mDocument.Layers().size())
            return false;
        mDocument.Remove(index);
        return true;
    }

    DeleteTerrainLayerCommand::DeleteTerrainLayerCommand(TerrainLayerDocument &document, uint32_t sequence)
        : TerrainLayerCommand(document), mSequence(sequence)
    {
    }

    bool DeleteTerrainLayerCommand::Do(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size())
            return false;
        mIndex = index;
        mRemoved = mDocument.Remove(index);
        return true;
    }

    bool DeleteTerrainLayerCommand::Undo(CommandContext &)
    {
        if (mRemoved.info.sequence != mSequence || mDocument.IndexOf(mSequence) < mDocument.Layers().size())
            return false;
        mDocument.Insert(mIndex, mRemoved);
        return true;
    }

    RenameTerrainLayerCommand::RenameTerrainLayerCommand(TerrainLayerDocument &document, uint32_t sequence,
        std::string name)
        : TerrainLayerCommand(document), mSequence(sequence), mName(std::move(name))
    {
    }

    bool RenameTerrainLayerCommand::Do(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size() || !mDocument.CanRename(mSequence, mName))
            return false;

        mBefore = mDocument.Layers()[index].info;
        TerrainEditLayerInfo renamed = mBefore;
        renamed.name = mName;
        mDocument.SetInfo(index, renamed);
        return true;
    }

    bool RenameTerrainLayerCommand::Undo(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size())
            return false;
        mDocument.SetInfo(index, mBefore);
        return true;
    }

    SetTerrainLayerPriorityCommand::SetTerrainLayerPriorityCommand(TerrainLayerDocument &document, uint32_t sequence,
        int32_t priority)
        : TerrainLayerCommand(document), mSequence(sequence), mPriority(priority)
    {
    }

    bool SetTerrainLayerPriorityCommand::Do(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size())
            return false;

        TerrainEditLayerInfo info = mDocument.Layers()[index].info;
        mBefore = info.priority;
        info.priority = mPriority;
        mDocument.SetInfo(index, info);
        return true;
    }

    bool SetTerrainLayerPriorityCommand::Undo(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size())
            return false;

        TerrainEditLayerInfo info = mDocument.Layers()[index].info;
        info.priority = mBefore;
        mDocument.SetInfo(index, info);
        return true;
    }

    SetTerrainLayerEnabledCommand::SetTerrainLayerEnabledCommand(TerrainLayerDocument &document, uint32_t sequence,
        bool enabled)
        : TerrainLayerCommand(document), mSequence(sequence), mEnabled(enabled)
    {
    }

    bool SetTerrainLayerEnabledCommand::Do(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size())
            return false;

        TerrainEditLayerInfo info = mDocument.Layers()[index].info;
        mBefore = info.enabled;
        info.enabled = mEnabled;
        mDocument.SetInfo(index, info);
        return true;
    }

    bool SetTerrainLayerEnabledCommand::Undo(CommandContext &)
    {
        const std::size_t index = mDocument.IndexOf(mSequence);
        if (index >= mDocument.Layers().size())
            return false;

        TerrainEditLayerInfo info = mDocument.Layers()[index].info;
        info.enabled = mBefore;
        mDocument.SetInfo(index, info);
        return true;
    }

    MoveTerrainLayerCommand::MoveTerrainLayerCommand(TerrainLayerDocument &document, uint32_t sequence,
        TerrainLayerMove move)
        : TerrainLayerCommand(document), mSequence(sequence), mMove(move)
    {
    }

    bool MoveTerrainLayerCommand::Do(CommandContext &)
    {
        const std::size_t movedIndex = mDocument.IndexOf(mSequence);
        if (movedIndex >= mDocument.Layers().size())
            return false;

        const std::vector<std::size_t> order = mDocument.CompositingOrder();
        const auto position = std::find(order.begin(), order.end(), movedIndex);
        if (position == order.end())
            return false;

        const std::size_t at = static_cast<std::size_t>(position - order.begin());
        std::size_t neighbourIndex = 0;
        if (mMove == TerrainLayerMove::Up)
        {
            if (at + 1 >= order.size())
                return false;
            neighbourIndex = order[at + 1];
        }
        else
        {
            if (at == 0)
                return false;
            neighbourIndex = order[at - 1];
        }

        TerrainEditLayerInfo moved = mDocument.Layers()[movedIndex].info;
        TerrainEditLayerInfo neighbour = mDocument.Layers()[neighbourIndex].info;
        mMovedBefore = moved.priority;
        mNeighbourBefore = neighbour.priority;
        mNeighbour = neighbour.sequence;

        if (moved.priority == neighbour.priority)
        {
            if (mMove == TerrainLayerMove::Up)
            {
                if (moved.priority == std::numeric_limits<int32_t>::max())
                    return false;
                ++moved.priority;
            }
            else
            {
                if (moved.priority == std::numeric_limits<int32_t>::min())
                    return false;
                --moved.priority;
            }
        }
        else
            std::swap(moved.priority, neighbour.priority);

        mDocument.SetInfo(movedIndex, moved);
        mDocument.SetInfo(neighbourIndex, neighbour);
        return true;
    }

    bool MoveTerrainLayerCommand::Undo(CommandContext &)
    {
        const std::size_t movedIndex = mDocument.IndexOf(mSequence);
        const std::size_t neighbourIndex = mDocument.IndexOf(mNeighbour);
        if (movedIndex >= mDocument.Layers().size() || neighbourIndex >= mDocument.Layers().size())
            return false;

        TerrainEditLayerInfo moved = mDocument.Layers()[movedIndex].info;
        TerrainEditLayerInfo neighbour = mDocument.Layers()[neighbourIndex].info;
        moved.priority = mMovedBefore;
        neighbour.priority = mNeighbourBefore;
        mDocument.SetInfo(movedIndex, moved);
        mDocument.SetInfo(neighbourIndex, neighbour);
        return true;
    }

    TerrainBrushCommand::TerrainBrushCommand(TerrainLayerDocument &document, uint32_t sequence,
        TerrainEditTileStates before, TerrainEditTileStates after)
        : TerrainLayerCommand(document), mSequence(sequence), mBefore(std::move(before)), mAfter(std::move(after))
    {
    }

    bool TerrainBrushCommand::Do(CommandContext &)
    {
        return Apply(mAfter);
    }

    bool TerrainBrushCommand::Undo(CommandContext &)
    {
        return Apply(mBefore);
    }

    std::size_t TerrainBrushCommand::Size() const
    {
        const auto bytes = [](const TerrainEditTileStates &states) {
            std::size_t size = 0;
            for (const auto &[tile, value] : states)
            {
                (void)tile;
                if (value)
                    size += sizeof(TerrainEditTile) + (value->values.size() + value->alpha.size()) * sizeof(float);
            }
            return size;
        };
        return sizeof(*this) + bytes(mBefore) + bytes(mAfter);
    }

    bool TerrainBrushCommand::Apply(const TerrainEditTileStates &states)
    {
        TerrainEditLayer *layer = mDocument.MutableLayer(mSequence);
        if (layer == nullptr)
            return false;

        for (const auto &[tile, value] : states)
        {
            if (value)
                layer->tiles[tile] = *value;
            else
                layer->tiles.erase(tile);
        }
        mDocument.MarkChanged();
        return true;
    }
} // namespace mir
