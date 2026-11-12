/**
 * @file TerrainLayerCommands.h
 * @author Jackey Cen
 * @brief Undoable add, delete, rename, priority, move and enable changes to an Edit Layer stack.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>
#include <terrain/TerrainEditLayer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    class TerrainLayerDocument;

    // An Edit Layer stack is saved apart from the scene, so no command moves the scene document.
    class TerrainLayerCommand : public ICommand
    {
    public:
        void CollectIds(std::vector<HistoryId> &out) const override { (void)out; }
        void CollectDocuments(std::vector<DocumentId> &out) const override { (void)out; }

    protected:
        explicit TerrainLayerCommand(TerrainLayerDocument &document) : mDocument(document) {}

        TerrainLayerDocument &mDocument;
    };

    class AddTerrainLayerCommand : public TerrainLayerCommand
    {
    public:
        AddTerrainLayerCommand(TerrainLayerDocument &document, TerrainEditBlend blend);

        bool Do(CommandContext &context) override;
        bool Undo(CommandContext &context) override;
        std::size_t Size() const override { return sizeof(*this); }
        std::string Name() const override { return "Add Edit Layer"; }

    private:
        TerrainEditBlend mBlend;
        TerrainEditLayer mLayer;
        bool mCreated = false;
    };

    class DeleteTerrainLayerCommand : public TerrainLayerCommand
    {
    public:
        DeleteTerrainLayerCommand(TerrainLayerDocument &document, uint32_t sequence);

        bool Do(CommandContext &context) override;
        bool Undo(CommandContext &context) override;
        std::size_t Size() const override { return sizeof(*this) + mRemoved.tiles.size() * sizeof(TerrainEditTile); }
        std::string Name() const override { return "Delete Edit Layer"; }

    private:
        uint32_t mSequence;
        std::size_t mIndex = 0;
        TerrainEditLayer mRemoved;
    };

    class RenameTerrainLayerCommand : public TerrainLayerCommand
    {
    public:
        RenameTerrainLayerCommand(TerrainLayerDocument &document, uint32_t sequence, std::string name);

        bool Do(CommandContext &context) override;
        bool Undo(CommandContext &context) override;
        std::size_t Size() const override { return sizeof(*this) + mName.size(); }
        std::string Name() const override { return "Rename Edit Layer"; }

    private:
        uint32_t mSequence;
        std::string mName;
        TerrainEditLayerInfo mBefore;
    };

    class SetTerrainLayerPriorityCommand : public TerrainLayerCommand
    {
    public:
        SetTerrainLayerPriorityCommand(TerrainLayerDocument &document, uint32_t sequence, int32_t priority);

        bool Do(CommandContext &context) override;
        bool Undo(CommandContext &context) override;
        std::size_t Size() const override { return sizeof(*this); }
        std::string Name() const override { return "Set Edit Layer Priority"; }

    private:
        uint32_t mSequence;
        int32_t mPriority;
        int32_t mBefore = 0;
    };

    class SetTerrainLayerEnabledCommand : public TerrainLayerCommand
    {
    public:
        SetTerrainLayerEnabledCommand(TerrainLayerDocument &document, uint32_t sequence, bool enabled);

        bool Do(CommandContext &context) override;
        bool Undo(CommandContext &context) override;
        std::size_t Size() const override { return sizeof(*this); }
        std::string Name() const override { return mEnabled ? "Enable Edit Layer" : "Disable Edit Layer"; }

    private:
        uint32_t mSequence;
        bool mEnabled;
        bool mBefore = true;
    };

    enum class TerrainLayerMove : uint8_t
    {
        Up,
        Down,
    };

    // Swaps priorities with the neighbour in compositing order; equal priorities step past it instead.
    class MoveTerrainLayerCommand : public TerrainLayerCommand
    {
    public:
        MoveTerrainLayerCommand(TerrainLayerDocument &document, uint32_t sequence, TerrainLayerMove move);

        bool Do(CommandContext &context) override;
        bool Undo(CommandContext &context) override;
        std::size_t Size() const override { return sizeof(*this); }
        std::string Name() const override
        {
            return mMove == TerrainLayerMove::Up ? "Move Edit Layer Up" : "Move Edit Layer Down";
        }

    private:
        uint32_t mSequence;
        TerrainLayerMove mMove;
        uint32_t mNeighbour = 0;
        int32_t mMovedBefore = 0;
        int32_t mNeighbourBefore = 0;
    };
} // namespace mir
