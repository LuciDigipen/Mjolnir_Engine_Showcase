/**
 * @file ComponentCommands.cpp
 * @author Dong Ta
 * @brief Field edits, add component and remove component as undoable commands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/ComponentCommands.h>

#include "SubtreeRecord.h"

#include <core/ecs/ComponentFields.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <cstring>
#include <format>
#include <utility>

namespace mir
{
    namespace
    {
        static_assert(FieldSize(FieldKind::Mat4) <= sizeof(FieldValue::mBytes));
        static_assert(kFieldStringCapacity <= sizeof(FieldValue::mBytes));

        
        struct Stored
        {
            FieldValue mValue;
            HistoryId mRef = kNullHistoryId;
        };

        Stored Keep(CommandContext &context, const ComponentOps &ops, const FieldValue &value)
        {
            Stored stored{ value };
            if (ops.mFields[value.mField].mKind == FieldKind::EntityRef)
            {
                Entity target = kNullEntity;
                std::memcpy(&target, value.mBytes, sizeof(Entity));
                stored.mRef = context.mWorld.IsAlive(target) ? context.mIds.IdOf(target) : kNullHistoryId;
            }
            return stored;
        }

        FieldValue ReadField(const ComponentOps &ops, const void *component, uint32_t field)
        {
            FieldValue value;
            value.mField = field;
            ops.mFields[field].Read(component, value.mBytes);
            return value;
        }

        //using field's setter, so Transform bumps its version
        void WriteField(CommandContext &context, const ComponentOps &ops, void *component, const Stored &stored)
        {
            const FieldDesc &field = ops.mFields[stored.mValue.mField];
            if (field.mKind == FieldKind::EntityRef)
            {
                const Entity target = LiveEntity(context.mWorld, context.mIds, stored.mRef);
                field.Write(component, &target);
                return;
            }
            field.Write(component, stored.mValue.mBytes);
        }

        bool Same(const ComponentOps &ops, const Stored &a, const Stored &b)
        {
            const FieldDesc &field = ops.mFields[a.mValue.mField];
            switch (field.mKind)
            {
            case FieldKind::EntityRef:
                return a.mRef == b.mRef;
            case FieldKind::String:
                return std::strncmp(reinterpret_cast<const char *>(a.mValue.mBytes),
                    reinterpret_cast<const char *>(b.mValue.mBytes), kFieldStringCapacity) == 0;
            default:
                return std::memcmp(a.mValue.mBytes, b.mValue.mBytes, FieldSize(field.mKind)) == 0;
            }
        }

        bool Writable(const ComponentOps &ops, uint32_t field)
        {
            return field < ops.mFields.size() && !ops.mFields[field].ReadOnly();
        }

        //null when the entity is gone or lacks the component
        void *LiveComponent(CommandContext &context, const ComponentOps &ops, HistoryId id)
        {
            const Entity entity = LiveEntity(context.mWorld, context.mIds, id);
            if (entity.IsNull() || !ops.Has(context.mWorld, entity))
                return nullptr;
            return ops.GetComponent(context.mWorld, entity);
        }

        void CollectRefs(const std::vector<Stored> &values, std::vector<HistoryId> &out)
        {
            for (const Stored &stored : values)
                out.push_back(stored.mRef);
        }



        class SetFieldsCommand final : public ICommand
        {

        public:
            SetFieldsCommand(Entity entity, const ComponentOps &ops, std::vector<FieldValue> values, std::string name)
                : mTarget(entity), mOps(&ops), mValues(std::move(values)), mName(std::move(name))
            {
            }

            bool Do(CommandContext &context) override
            {
                if (mEntity == kNullHistoryId && !First(context))
                    return false;

                void *component = LiveComponent(context, *mOps, mEntity);
                if (component == nullptr)
                    return false;

                for (const Change &change : mChanges)
                    WriteField(context, *mOps, component, change.mAfter);
                return true;
            }

            bool Undo(CommandContext &context) override
            {
                if (void *component = LiveComponent(context, *mOps, mEntity))
                {
                    for (const Change &change : mChanges)
                        WriteField(context, *mOps, component, change.mBefore);
                }
                return true;
            }

            std::size_t Size() const override { return sizeof(*this) + mChanges.size() * sizeof(Change) + mName.size(); }
            std::string Name() const override { return mName; }

            void CollectIds(std::vector<HistoryId> &out) const override
            {
                out.push_back(mEntity);
                for (const Change &change : mChanges)
                {
                    out.push_back(change.mBefore.mRef);
                    out.push_back(change.mAfter.mRef);
                }
            }

           
            bool TryMerge(ICommand &next) override
            {
                const auto *other = dynamic_cast<const SetFieldsCommand *>(&next);
                if (other == nullptr || other->mEntity != mEntity || other->mOps != mOps ||
                    other->mChanges.size() != mChanges.size())
                    return false;

                for (std::size_t i = 0; i < mChanges.size(); ++i)
                {
                    if (other->mChanges[i].mField != mChanges[i].mField)
                        return false;
                }
                for (std::size_t i = 0; i < mChanges.size(); ++i)
                    mChanges[i].mAfter = other->mChanges[i].mAfter;
                return true;
            }

        private:
            struct Change
            {
                uint32_t mField = 0;
                Stored mBefore;
                Stored mAfter;
            };

            bool First(CommandContext &context)
            {
                World &world = context.mWorld;
                if (!world.IsAlive(mTarget) || !mOps->Has(world, mTarget))
                    return false;

                const void *component = mOps->GetComponent(world, mTarget);
                if (component == nullptr)
                    return false;

                bool changed = false;
                for (const FieldValue &value : mValues)
                {
                    if (!Writable(*mOps, value.mField))
                        continue;

                    Change change{ value.mField, Keep(context, *mOps, ReadField(*mOps, component, value.mField)),
                        Keep(context, *mOps, value) };
                    changed |= !Same(*mOps, change.mBefore, change.mAfter);
                    mChanges.push_back(change);
                }
                mValues.clear();

                
                if (!changed)
                {
                    mChanges.clear();
                    return false;
                }

                mEntity = context.mIds.IdOf(mTarget);
                if (mName.empty())
                {
                    mName = mChanges.size() == 1
                        ? std::format("Edit {} {}", mOps->mType.name, mOps->mFields[mChanges.front().mField].mName)
                        : std::format("Edit {}", mOps->mType.name);
                }
                return true;
            }

            Entity mTarget;
            const ComponentOps *mOps;
            std::vector<FieldValue> mValues;
            std::string mName;

            HistoryId mEntity = kNullHistoryId;
            std::vector<Change> mChanges;
        };

        class AddComponentCommand final : public ICommand
        {
        public:
            AddComponentCommand(Entity entity, const ComponentOps &ops, std::vector<FieldValue> values)
                : mTarget(entity), mOps(&ops), mValues(std::move(values))
            {
                mName = std::format("{} {}", mValues.empty() ? "Add" : "Paste", ops.mType.name);
            }

            bool Do(CommandContext &context) override
            {
                World &world = context.mWorld;
                if (mEntity == kNullHistoryId)
                {
                    if (!world.IsAlive(mTarget) || mOps->Has(world, mTarget))
                        return false;

                    mEntity = context.mIds.IdOf(mTarget);
                    for (const FieldValue &value : mValues)
                    {
                        if (Writable(*mOps, value.mField))
                            mFields.push_back(Keep(context, *mOps, value));
                    }
                    mValues.clear();
                }

                const Entity entity = LiveEntity(world, context.mIds, mEntity);
                if (entity.IsNull() || mOps->Has(world, entity))
                    return false;

                mOps->AddDefault(world, entity);
                if (void *component = mOps->GetComponent(world, entity))
                {
                    for (const Stored &stored : mFields)
                        WriteField(context, *mOps, component, stored);
                }
                return true;
            }

            bool Undo(CommandContext &context) override
            {
                const Entity entity = LiveEntity(context.mWorld, context.mIds, mEntity);
                if (!entity.IsNull() && mOps->Has(context.mWorld, entity))
                    mOps->Remove(context.mWorld, entity);
                return true;
            }

            std::size_t Size() const override { return sizeof(*this) + mFields.size() * sizeof(Stored) + mName.size(); }
            std::string Name() const override { return mName; }

            void CollectIds(std::vector<HistoryId> &out) const override
            {
                out.push_back(mEntity);
                CollectRefs(mFields, out);
            }

        private:
            Entity mTarget;
            const ComponentOps *mOps;
            std::vector<FieldValue> mValues;
            std::string mName;

            HistoryId mEntity = kNullHistoryId;
            std::vector<Stored> mFields;
        };

        class RemoveComponentCommand final : public ICommand
        {
        public:
            RemoveComponentCommand(Entity entity, const ComponentOps &ops)
                : mTarget(entity), mOps(&ops), mName(std::format("Remove {}", ops.mType.name))
            {
            }

            bool Do(CommandContext &context) override
            {
                World &world = context.mWorld;
                if (mEntity == kNullHistoryId)
                {
                    if (!world.IsAlive(mTarget) || !mOps->Has(world, mTarget))
                        return false;
                    mEntity = context.mIds.IdOf(mTarget);
                }

                const Entity entity = LiveEntity(world, context.mIds, mEntity);
                if (entity.IsNull() || !mOps->Has(world, entity))
                    return false;

                //every writable field, hidden ones included, since those are saved too
                mFields.clear();
                if (const void *component = mOps->GetComponent(world, entity))
                {
                    for (uint32_t field = 0; field < mOps->mFields.size(); ++field)
                    {
                        if (Writable(*mOps, field))
                            mFields.push_back(Keep(context, *mOps, ReadField(*mOps, component, field)));
                    }
                }

                mOps->Remove(world, entity);
                return true;
            }

            bool Undo(CommandContext &context) override
            {
                World &world = context.mWorld;
                const Entity entity = LiveEntity(world, context.mIds, mEntity);
                if (entity.IsNull() || mOps->Has(world, entity))
                    return true;

                mOps->AddDefault(world, entity);
                if (void *component = mOps->GetComponent(world, entity))
                {
                    for (const Stored &stored : mFields)
                        WriteField(context, *mOps, component, stored);
                }
                return true;
            }

            std::size_t Size() const override { return sizeof(*this) + mFields.size() * sizeof(Stored) + mName.size(); }
            std::string Name() const override { return mName; }

            void CollectIds(std::vector<HistoryId> &out) const override
            {
                out.push_back(mEntity);
                CollectRefs(mFields, out);
            }

        private:
            Entity mTarget;
            const ComponentOps *mOps;
            std::string mName;

            HistoryId mEntity = kNullHistoryId;
            std::vector<Stored> mFields;
        };
    }

    std::unique_ptr<ICommand> MakeSetFieldsCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name)
    {
        return std::make_unique<SetFieldsCommand>(entity, ops, std::move(values), std::move(name));
    }

    std::unique_ptr<ICommand> MakeAddComponentCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values)
    {
        return std::make_unique<AddComponentCommand>(entity, ops, std::move(values));
    }

    std::unique_ptr<ICommand> MakeRemoveComponentCommand(Entity entity, const ComponentOps &ops)
    {
        return std::make_unique<RemoveComponentCommand>(entity, ops);
    }
}
