/**
 * @file EntityCommands.cpp
 * @author Dong Ta
 * @brief Create, delete, duplicate and reparent as undoable commands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/EntityCommands.h>

#include "SubtreeRecord.h"

#include <editor/EntityBaseline.h>
#include <editor/Selection.h>
#include <editor/history/SceneOps.h>
#include <editor/panels/HierarchyPanel.h>

#include <core/ecs/EntityPresetRegistry.h>
#include <core/ecs/TransformHierarchy.h>
#include <core/ecs/World.h>
#include <core/ecs/components/Name.h>
#include <core/ecs/components/Transform.h>
#include <core/log/Log.h>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        std::string NameOf(World &world, Entity entity)
        {
            const Name *name = world.GetComponent<Name>(entity);
            return name != nullptr ? std::string(name->mValue) : std::string("Entity");
        }

        void SelectOnly(World &world, std::vector<Entity> entities)
        {
            if (Selection *selection = world.TryResource<Selection>())
                selection->mSelected = std::move(entities);
        }

        std::optional<Transform> ReadTransform(World &world, Entity entity)
        {
            if (const Transform *transform = world.GetComponent<Transform>(entity))
                return *transform;
            return std::nullopt;
        }

        
        void WriteTransform(World &world, Entity entity, const std::optional<Transform> &saved)
        {
            Transform *transform = world.GetComponent<Transform>(entity);
            if (transform == nullptr || !saved)
                return;

            transform->SetPosition(saved->Position());
            transform->SetRotation(saved->Rotation());
            transform->SetScale(saved->Scale());
        }

        
        void MoveTo(World &world, Entity entity, Entity parent, Entity before)
        {
            if (before.IsNull() && ParentOf(world, entity) == parent)
                MoveToLastSibling(world, entity);
            else
                SetParent(world, entity, parent, before, false);
        }

        //create and duplicate: the first Do makes the entity, undo removes it, redo restores it
        class MakeEntityCommandBase : public ICommand
        {
        public:
            bool Do(CommandContext &context) 
            {
                Entity entity = kNullEntity;
                if (mRoot == kNullHistoryId)
                {
                    entity = Make(context);
                    if (entity.IsNull())
                        return false;
                    mRoot = context.mIds.IdOf(entity);
                }
                else
                {
                    entity = mRecord.Restore(context);
                    if (entity.IsNull())
                        return false;
                }

                SelectOnly(context.mWorld, { entity });
                return true;
            }

            void Undo(CommandContext &context) 
            {
                if (!mRecord.Remove(context, context.mIds.EntityOf(mRoot)))
                    MIR_LOG_WARN("History: '{}' cannot be undone, the entity is gone", mName);
            }

            std::size_t Size() const final { return sizeof(*this) + mRecord.Size() + mName.size(); }
            std::string Name() const final { return mName; }

            void CollectIds(std::vector<HistoryId> &out) const 
            {
                out.push_back(mRoot);
                mRecord.CollectIds(out);
            }

        protected:
            virtual Entity Make(CommandContext &context) = 0;

            std::string mName;

        private:
            HistoryId mRoot = kNullHistoryId;
            SubtreeRecord mRecord;
        };

        class CreateCommand final : public MakeEntityCommandBase
        {
        public:
            explicit CreateCommand(const EntityPresetDesc *preset) : mPreset(preset)
            {
                mName = preset != nullptr
                    ? std::format("Create {}", preset->mName.substr(preset->mName.find('/') + 1))
                    : std::string("Create Entity");
            }

        private:
            Entity Make(CommandContext &context) override
            {
                World &world = context.mWorld;
                if (mPreset != nullptr && mPreset->mCanCreate != nullptr && !mPreset->mCanCreate(world))
                    return kNullEntity;

                const Entity entity = context.mScene.CreateEntity();
                if (mPreset != nullptr && mPreset->mCreate != nullptr)
                    mPreset->mCreate(world, entity);

                //after the preset
                AddMissingBaseline(world, entity);
                return entity;
            }

            const EntityPresetDesc *mPreset;
        };

        class DuplicateCommand final : public MakeEntityCommandBase
        {
        public:
            explicit DuplicateCommand(Entity source) : mSource(source) {}

        private:
            Entity Make(CommandContext &context) override
            {
                if (!context.mWorld.IsAlive(mSource))
                    return kNullEntity;

                mName = std::format("Duplicate {}", NameOf(context.mWorld, mSource));
                return context.mScene.Duplicate(mSource);
            }

            Entity mSource;
        };

        class DeleteCommand final : public ICommand
        {
        public:
            explicit DeleteCommand(Entity target) : mTarget(target) {}

            bool Do(CommandContext &context) override
            {
                World &world = context.mWorld;
                if (mRoot == kNullHistoryId)
                {
                    if (!world.IsAlive(mTarget))
                        return false;
                    mRoot = context.mIds.IdOf(mTarget);
                    mName = std::format("Delete {}", NameOf(world, mTarget));
                }

                const Entity target = context.mIds.EntityOf(mRoot);
                const Selection *selection = world.TryResource<Selection>();
                const bool selected = selection != nullptr && selection->Contains(target);
                const Entity neighbor = NeighborInHierarchy(world, target);

                if (!mRecord.Remove(context, target))
                    return false;

                if (selected)
                    SelectOnly(world, neighbor.IsNull() ? std::vector<Entity>{} : std::vector<Entity>{ neighbor });
                return true;
            }

            void Undo(CommandContext &context) override
            {
                if (mRecord.Restore(context).IsNull())
                    MIR_LOG_WARN("History: '{}' cannot be undone, nothing was restored", mName);
            }

            std::size_t Size() const override { return sizeof(*this) + mRecord.Size() + mName.size(); }
            std::string Name() const override { return mName; }

            void CollectIds(std::vector<HistoryId> &out) const override
            {
                out.push_back(mRoot);
                mRecord.CollectIds(out);
            }

        private:
            Entity mTarget;
            HistoryId mRoot = kNullHistoryId;
            SubtreeRecord mRecord;
            std::string mName;
        };

        class ReParentCommand final : public ICommand
        {
        public:
            ReParentCommand(std::vector<Entity> targets, Entity parent, Entity before)
                : mTargets(std::move(targets)), mParentEntity(parent), mBeforeEntity(before)
            {
            }

            bool Do(CommandContext &context) override
            {
                if (mMoves.empty())
                    return First(context);

                World &world = context.mWorld;
                const Entity parent = LiveEntity(world, context.mIds, mParent);
                const Entity before = LiveEntity(world, context.mIds, mBefore);

                std::vector<Entity> moved;
                for (const Move &move : mMoves)
                {
                    const Entity entity = LiveEntity(world, context.mIds, move.mEntity);
                    if (entity.IsNull())
                        continue;

                    MoveTo(world, entity, parent, before);
                    WriteTransform(world, entity, move.mAfter);
                    moved.push_back(entity);
                }
                SelectOnly(world, std::move(moved));
                return true;
            }

            
            void Undo(CommandContext &context) override
            {
                World &world = context.mWorld;
                for (auto it = mMoves.rbegin(); it != mMoves.rend(); ++it)
                {
                    const Entity entity = LiveEntity(world, context.mIds, it->mEntity);
                    if (entity.IsNull())
                        continue;

                    MoveTo(world, entity, LiveEntity(world, context.mIds, it->mOldParent),
                        LiveEntity(world, context.mIds, it->mOldNext));
                    WriteTransform(world, entity, it->mBefore);
                }
            }

            std::size_t Size() const override
            {
                return sizeof(*this) + mMoves.size() * sizeof(Move) + mTargets.size() * sizeof(Entity) + mName.size();
            }

            std::string Name() const override { return mName; }

            void CollectIds(std::vector<HistoryId> &out) const override
            {
                out.push_back(mParent);
                out.push_back(mBefore);
                for (const Move &move : mMoves)
                {
                    out.push_back(move.mEntity);
                    out.push_back(move.mOldParent);
                    out.push_back(move.mOldNext);
                }
            }

        private:
            struct Move
            {
                HistoryId mEntity = kNullHistoryId;
                HistoryId mOldParent = kNullHistoryId;
                HistoryId mOldNext = kNullHistoryId;
                std::optional<Transform> mBefore;
                std::optional<Transform> mAfter;
            };

            bool First(CommandContext &context)
            {
                World &world = context.mWorld;

                
                if (!CanSetParentAll(world, mTargets, mParentEntity, mBeforeEntity))
                    return false;

                const std::vector<Entity> moved = TopmostInHierarchyOrder(world, mTargets);
                for (const Entity entity : moved)
                {
                    Move move;
                    move.mEntity = context.mIds.IdOf(entity);
                    move.mOldParent = context.mIds.IdOf(ParentOf(world, entity));
                    move.mOldNext = context.mIds.IdOf(NextSibling(world, entity));
                    move.mBefore = ReadTransform(world, entity);

                    //keeps the world pose
                    if (mBeforeEntity.IsNull() && ParentOf(world, entity) == mParentEntity)
                        MoveToLastSibling(world, entity);
                    else
                        SetParent(world, entity, mParentEntity, mBeforeEntity);

                    move.mAfter = ReadTransform(world, entity);
                    mMoves.push_back(std::move(move));
                }
                if (mMoves.empty())
                    return false;

                mParent = context.mIds.IdOf(mParentEntity);
                mBefore = context.mIds.IdOf(mBeforeEntity);
                mName = moved.size() == 1 ? std::format("Move {}", NameOf(world, moved.front()))
                                          : std::format("Move {} Entities", moved.size());
                mTargets.clear();

                SelectOnly(world, moved);
                return true;
            }

            std::vector<Entity> mTargets;
            Entity mParentEntity;
            Entity mBeforeEntity;

            HistoryId mParent = kNullHistoryId;
            HistoryId mBefore = kNullHistoryId;
            std::vector<Move> mMoves;
            std::string mName;
        };
    }

    std::unique_ptr<ICommand> MakeCreateCommand(const EntityPresetDesc *preset)
    {
        return std::make_unique<CreateCommand>(preset);
    }

    std::unique_ptr<ICommand> MakeDeleteCommand(Entity target)
    {
        return std::make_unique<DeleteCommand>(target);
    }

    std::unique_ptr<ICommand> MakeDuplicateCommand(Entity source)
    {
        return std::make_unique<DuplicateCommand>(source);
    }

    std::unique_ptr<ICommand> MakeReParentCommand(std::vector<Entity> targets, Entity parent, Entity before)
    {
        return std::make_unique<ReParentCommand>(std::move(targets), parent, before);
    }

    std::unique_ptr<ICommand> MakeEntityCommand(const EntityAction &action)
    {
        switch (action.mKind)
        {
        case EntityActionKind::Create:
            return MakeCreateCommand(action.mPreset);
        case EntityActionKind::Delete:
            return MakeDeleteCommand(action.mTarget);
        case EntityActionKind::Duplicate:
            return MakeDuplicateCommand(action.mTarget);
        case EntityActionKind::Reparent:
            return MakeReParentCommand(action.mTargets, action.mParent, action.mBefore);
        case EntityActionKind::None:
            break;
        }
        return nullptr;
    }
}
