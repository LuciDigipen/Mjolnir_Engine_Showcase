/**
 * @file History.tests.cpp
 * @author Dong Ta
 * @brief History cursor, budget, selection and id tests, run with a fake command.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/Selection.h>
#include <editor/history/CommandGroup.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using mir::CapturedSubtree;
    using mir::CommandContext;
    using mir::Entity;
    using mir::EntityIds;
    using mir::History;
    using mir::HistoryId;
    using mir::kNullEntity;
    using mir::kNullHistoryId;
    using mir::Selection;
    using mir::World;

    struct Value
    {
        int mValue = 0;
    };

    // sets Value, and selects an entity like the entity commands do
    class SetValue final : public mir::ICommand
    {
    public:
        explicit SetValue(int value, Entity select = kNullEntity, std::size_t size = 16, bool refuse = false)
            : mAfter(value), mSelect(select), mSize(size), mRefuse(refuse)
        {
        }

        bool Do(CommandContext &context) override
        {
            if (mRefuse)
                return false;

            Value &value = context.mWorld.Resource<Value>();
            if (!mDone)
            {
                mBefore = value.mValue;
                mSelectId = context.mIds.IdOf(mSelect);
                mDone = true;
            }

            value.mValue = mAfter;
            if (mSelectId != kNullHistoryId)
                context.mWorld.Resource<Selection>().mSelected = { context.mIds.EntityOf(mSelectId) };
            return true;
        }

        bool Undo(CommandContext &context) override
        {
            context.mWorld.Resource<Value>().mValue = mBefore;
            return true;
        }

        std::size_t Size() const override { return mSize; }
        std::string Name() const override { return "Set " + std::to_string(mAfter); }

        void CollectIds(std::vector<HistoryId> &out) const override
        {
            if (mSelectId != kNullHistoryId)
                out.push_back(mSelectId);
        }

    private:
        int mBefore = 0;
        int mAfter;
        Entity mSelect;
        HistoryId mSelectId = kNullHistoryId;
        std::size_t mSize;
        bool mRefuse;
        bool mDone = false;
    };

    // sets Value too, but folds into an open step for the same key, as a field drag does
    class DragValue final : public mir::ICommand
    {
    public:
        DragValue(int key, int value) : mKey(key), mAfter(value) {}

        bool Do(CommandContext &context) override
        {
            Value &value = context.mWorld.Resource<Value>();
            if (!mDone)
            {
                mBefore = value.mValue;
                mDone = true;
            }
            value.mValue = mAfter;
            return true;
        }

        bool Undo(CommandContext &context) override
        {
            context.mWorld.Resource<Value>().mValue = mBefore;
            return true;
        }

        std::size_t Size() const override { return 16; }
        std::string Name() const override { return "Drag " + std::to_string(mKey); }
        void CollectIds(std::vector<HistoryId> &) const override {}

        bool TryMerge(mir::ICommand &next) override
        {
            const auto *other = dynamic_cast<const DragValue *>(&next);
            if (other == nullptr || other->mKey != mKey)
                return false;
            mAfter = other->mAfter;
            return true;
        }

    private:
        int mKey;
        int mBefore = 0;
        int mAfter;
        bool mDone = false;
    };

    struct Slots
    {
        int mValues[2]{};
    };

    // a DragValue on one of several slots, as each entity is in a gizmo drag over several
    class DragSlot final : public mir::ICommand
    {
    public:
        DragSlot(int slot, int value, bool refuse = false) : mSlot(slot), mAfter(value), mRefuse(refuse) {}

        bool Do(CommandContext &context) override
        {
            if (mRefuse)
                return false;

            int &value = context.mWorld.Resource<Slots>().mValues[mSlot];
            if (!mDone)
            {
                mBefore = value;
                mDone = true;
            }
            value = mAfter;
            return true;
        }

        bool Undo(CommandContext &context) override
        {
            context.mWorld.Resource<Slots>().mValues[mSlot] = mBefore;
            return true;
        }

        std::size_t Size() const override { return 16; }
        std::string Name() const override { return "Drag slot " + std::to_string(mSlot); }
        void CollectIds(std::vector<HistoryId> &) const override {}

        bool TryMerge(mir::ICommand &next) override
        {
            const auto *other = dynamic_cast<const DragSlot *>(&next);
            if (other == nullptr || other->mSlot != mSlot)
                return false;
            mAfter = other->mAfter;
            return true;
        }

    private:
        int mSlot;
        int mBefore = 0;
        int mAfter;
        bool mRefuse;
        bool mDone = false;
    };

    std::unique_ptr<mir::ICommand> SlotGroup(const char *name, std::initializer_list<std::pair<int, int>> slots)
    {
        std::vector<std::unique_ptr<mir::ICommand>> commands;
        for (const auto &[slot, value] : slots)
            commands.push_back(std::make_unique<DragSlot>(slot, value));
        return mir::MakeCommandGroup(name, std::move(commands));
    }

    class NoSceneOps final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        Entity CopyAsOrdinaryObject(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
        Entity PlacePrefab(std::string_view, Entity, const mir::WorldVec3 &) override { return kNullEntity; }
    };

    struct Fixture
    {
        World mWorld;
        NoSceneOps mScene;
        History mHistory;

        explicit Fixture(std::size_t budget = mir::kHistoryBudget) : mHistory(budget)
        {
            mWorld.EmplaceResource<Value>();
            mWorld.EmplaceResource<Slots>();
            mWorld.EmplaceResource<Selection>();
        }

        void Run(std::unique_ptr<mir::ICommand> command)
        {
            mHistory.Submit(std::move(command));
            Flush();
        }

        void Set(int value, Entity select = kNullEntity)
        {
            mHistory.Submit(std::make_unique<SetValue>(value, select));
            Flush();
        }

        void Undo()
        {
            mHistory.RequestUndo();
            Flush();
        }

        void Redo()
        {
            mHistory.RequestRedo();
            Flush();
        }

        // one frame of a held widget
        void Drag(int key, int value)
        {
            mHistory.Submit(std::make_unique<DragValue>(key, value));
            Flush();
        }

        void Release()
        {
            mHistory.EndStep();
            Flush();
        }

        void Flush() { mHistory.Flush(mWorld, mScene); }
        int Current() { return mWorld.Resource<Value>().mValue; }
        int Slot(int slot) { return mWorld.Resource<Slots>().mValues[slot]; }
        std::vector<Entity> &Selected() { return mWorld.Resource<Selection>().mSelected; }
    };
}

TEST_CASE("A submitted command runs at Flush, not before", "[editor][history]")
{
    Fixture f;
    f.mHistory.Submit(std::make_unique<SetValue>(1));

    CHECK(f.Current() == 0);
    CHECK_FALSE(f.mHistory.CanUndo());

    f.Flush();

    CHECK(f.Current() == 1);
    CHECK(f.mHistory.StepCount() == 1);
    CHECK(f.mHistory.CanUndo());
}

TEST_CASE("Undo and redo move the cursor", "[editor][history]")
{
    Fixture f;
    f.Set(1);
    f.Set(2);

    f.Undo();
    CHECK(f.Current() == 1);
    CHECK(f.mHistory.Cursor() == 1);

    f.Undo();
    CHECK(f.Current() == 0);
    CHECK_FALSE(f.mHistory.CanUndo());

    f.Undo();
    CHECK(f.Current() == 0);

    f.Redo();
    CHECK(f.Current() == 1);

    f.Redo();
    CHECK(f.Current() == 2);
    CHECK_FALSE(f.mHistory.CanRedo());
}

TEST_CASE("Several requests in one frame all run", "[editor][history]")
{
    Fixture f;
    f.Set(1);
    f.Set(2);
    f.Set(3);

    f.mHistory.RequestUndo();
    f.mHistory.RequestUndo();
    f.Flush();
    CHECK(f.Current() == 1);

    f.mHistory.RequestRedo();
    f.mHistory.RequestUndo();
    f.Flush();
    CHECK(f.Current() == 1);
}

TEST_CASE("Two commands in one frame are two steps", "[editor][history]")
{
    Fixture f;
    f.mHistory.Submit(std::make_unique<SetValue>(1));
    f.mHistory.Submit(std::make_unique<SetValue>(2));
    f.Flush();

    CHECK(f.mHistory.StepCount() == 2);
    f.Undo();
    CHECK(f.Current() == 1);
}

TEST_CASE("A new change after an undo drops the redo steps", "[editor][history]")
{
    Fixture f;
    f.Set(1);
    f.Set(2);
    f.Undo();

    f.Set(3);

    CHECK(f.mHistory.StepCount() == 2);
    CHECK_FALSE(f.mHistory.CanRedo());
    f.Undo();
    CHECK(f.Current() == 1);
}

TEST_CASE("A refused command records no step and keeps the redo steps", "[editor][history]")
{
    Fixture f;
    f.Set(1);
    f.Undo();

    f.mHistory.Submit(std::make_unique<SetValue>(5, kNullEntity, 16, true));
    f.Flush();

    CHECK(f.Current() == 0);
    CHECK(f.mHistory.StepCount() == 1);
    CHECK(f.mHistory.CanRedo());
}

TEST_CASE("Undo and redo name their step", "[editor][history]")
{
    Fixture f;
    CHECK(f.mHistory.UndoName().empty());

    f.Set(1);
    f.Set(2);
    CHECK(f.mHistory.UndoName() == "Set 2");
    CHECK(f.mHistory.RedoName().empty());

    f.Undo();
    CHECK(f.mHistory.UndoName() == "Set 1");
    CHECK(f.mHistory.RedoName() == "Set 2");
}

TEST_CASE("Each step restores the selection it had", "[editor][history]")
{
    Fixture f;
    const Entity a = f.mWorld.CreateEntity();
    const Entity b = f.mWorld.CreateEntity();
    f.Selected() = { a };

    f.Set(1, b);
    CHECK(f.Selected() == std::vector<Entity>{ b });

    f.Undo();
    CHECK(f.Selected() == std::vector<Entity>{ a });

    f.Redo();
    CHECK(f.Selected() == std::vector<Entity>{ b });
}

TEST_CASE("A dead entity drops out of a restored selection", "[editor][history]")
{
    Fixture f;
    const Entity a = f.mWorld.CreateEntity();
    const Entity b = f.mWorld.CreateEntity();
    f.Selected() = { a, b };

    f.Set(1);
    f.mWorld.DestroyEntity(a);
    f.Undo();

    CHECK(f.Selected() == std::vector<Entity>{ b });
}

TEST_CASE("Over budget, the oldest steps go and the newest stays", "[editor][history]")
{
    const auto submit = [](Fixture &f, int value)
        {
            f.mHistory.Submit(std::make_unique<SetValue>(value, kNullEntity, 1000));
            f.Flush();
        };

    SECTION("two steps fit")
    {
        Fixture f(2500);
        for (int value = 1; value <= 5; ++value)
            submit(f, value);

        CHECK(f.mHistory.StepCount() == 2);
        CHECK(f.mHistory.UsedBytes() <= 2500);

        f.Undo();
        f.Undo();
        CHECK(f.Current() == 3);
        CHECK_FALSE(f.mHistory.CanUndo());
    }
    SECTION("one step over the budget alone")
    {
        Fixture f(10);
        submit(f, 1);
        submit(f, 2);

        CHECK(f.mHistory.StepCount() == 1);
        f.Undo();
        CHECK(f.Current() == 1);
    }
}

TEST_CASE("Clear empties history and leaves the World alone", "[editor][history]")
{
    Fixture f;
    f.Set(1);
    f.Set(2);
    f.Undo();

    f.mHistory.Clear();

    CHECK(f.Current() == 1);
    CHECK_FALSE(f.mHistory.CanUndo());
    CHECK_FALSE(f.mHistory.CanRedo());
    CHECK(f.mHistory.UsedBytes() == 0);
    CHECK(f.mHistory.Ids().Size() == 0);
}

TEST_CASE("Ids no step holds are dropped", "[editor][history]")
{
    Fixture f;
    const Entity a = f.mWorld.CreateEntity();

    f.Set(1, a);
    CHECK(f.mHistory.Ids().Size() == 1);

    f.Undo();
    f.Set(2);

    CHECK(f.mHistory.Ids().Size() == 0);
}

TEST_CASE("EntityIds keeps an id across Unbind and Rebind", "[editor][history]")
{
    World world;
    EntityIds ids;
    const Entity a = world.CreateEntity();
    const HistoryId id = ids.IdOf(a);

    CHECK(id != kNullHistoryId);
    CHECK(ids.IdOf(a) == id);
    CHECK(ids.IdOf(kNullEntity) == kNullHistoryId);

    ids.Unbind(id);
    CHECK(ids.EntityOf(id).IsNull());

    const Entity b = world.CreateEntity();
    ids.Rebind(id, b);
    CHECK(ids.EntityOf(id) == b);
    CHECK(ids.IdOf(b) == id);

    // a new entity never gets an id that was handed out before
    CHECK(ids.IdOf(world.CreateEntity()) != id);

    const std::vector<HistoryId> keep{ id };
    ids.Retain(keep);
    CHECK(ids.Size() == 1);
    CHECK(ids.EntityOf(id) == b);
}

TEST_CASE("A drag folds into one step, and undo goes back to where it started", "[editor][history]")
{
    Fixture f;
    f.Drag(1, 5);
    f.Drag(1, 6);
    f.Drag(1, 7);

    CHECK(f.mHistory.StepCount() == 1);
    CHECK(f.Current() == 7);

    f.Undo();
    CHECK(f.Current() == 0);

    f.Redo();
    CHECK(f.Current() == 7);
}

TEST_CASE("Two separate edits to the same field are two steps", "[editor][history]")
{
    Fixture f;

    SECTION("released in a later frame")
    {
        f.Drag(1, 5);
        f.Release();
    }
    SECTION("released in the frame of the change")
    {
        f.mHistory.Submit(std::make_unique<DragValue>(1, 5));
        f.mHistory.EndStep();
        f.Flush();
    }
    f.Drag(1, 6);
    f.Release();

    CHECK(f.mHistory.StepCount() == 2);
    f.Undo();
    CHECK(f.Current() == 5);
}

TEST_CASE("A drag on another field starts a new step", "[editor][history]")
{
    Fixture f;
    f.Drag(1, 5);
    f.Drag(2, 6);

    CHECK(f.mHistory.StepCount() == 2);
}

TEST_CASE("A group drag folds into one step over everything it moved", "[editor][history][group]")
{
    Fixture f;
    f.Run(SlotGroup("Move", { { 0, 1 }, { 1, 1 } }));
    f.Run(SlotGroup("Move", { { 0, 2 }, { 1, 2 } }));
    f.Release();

    CHECK(f.mHistory.StepCount() == 1);
    CHECK(f.mHistory.UndoName() == "Move");

    f.Undo();
    CHECK(f.Slot(0) == 0);
    CHECK(f.Slot(1) == 0);

    f.Redo();
    CHECK(f.Slot(0) == 2);
    CHECK(f.Slot(1) == 2);
}

TEST_CASE("A group drag stays one step when a frame adds a command or skips one", "[editor][history][group]")
{
    Fixture f;
    f.Run(SlotGroup("Move", { { 0, 1 } }));
    f.Run(SlotGroup("Move", { { 0, 2 }, { 1, 5 } }));

    std::vector<std::unique_ptr<mir::ICommand>> skipped;
    skipped.push_back(std::make_unique<DragSlot>(0, 3));
    skipped.push_back(std::make_unique<DragSlot>(1, 9, true));
    f.Run(mir::MakeCommandGroup("Move", std::move(skipped)));
    f.Release();

    CHECK(f.mHistory.StepCount() == 1);
    CHECK(f.Slot(0) == 3);
    CHECK(f.Slot(1) == 5);

    f.Undo();
    CHECK(f.Slot(0) == 0);
    CHECK(f.Slot(1) == 0);

    f.Redo();
    CHECK(f.Slot(0) == 3);
    CHECK(f.Slot(1) == 5);
}

TEST_CASE("Groups of another name, or after a release, are new steps", "[editor][history][group]")
{
    Fixture f;
    f.Run(SlotGroup("Move", { { 0, 1 } }));
    f.Run(SlotGroup("Rotate", { { 0, 2 } }));
    f.Release();
    f.Run(SlotGroup("Rotate", { { 0, 3 } }));

    CHECK(f.mHistory.StepCount() == 3);
    f.Undo();
    CHECK(f.Slot(0) == 2);
}

TEST_CASE("Undo and redo close the open step", "[editor][history]")
{
    Fixture f;
    f.Drag(1, 5);
    f.Undo();
    f.Redo();
    f.Drag(1, 6);

    CHECK(f.mHistory.StepCount() == 2);
    f.Undo();
    CHECK(f.Current() == 5);
}

TEST_CASE("A change shows the unsaved marker and a save clears it", "[editor][history][unsaved]")
{
    Fixture f;
    CHECK_FALSE(f.mHistory.IsUnsaved());

    f.Set(1);
    CHECK(f.mHistory.IsUnsaved());

    f.mHistory.SetUnsaved(false);
    CHECK_FALSE(f.mHistory.IsUnsaved());
}

TEST_CASE("Undoing back to the saved step clears the marker, and redoing past it shows it again",
    "[editor][history][unsaved]")
{
    Fixture f;
    f.Set(1);
    f.Set(2);
    f.mHistory.SetUnsaved(false);
    f.Set(3);
    CHECK(f.mHistory.IsUnsaved());

    f.Undo();
    CHECK_FALSE(f.mHistory.IsUnsaved());

    f.Undo();
    CHECK(f.mHistory.IsUnsaved());

    f.Redo();
    CHECK_FALSE(f.mHistory.IsUnsaved());

    f.Redo();
    CHECK(f.mHistory.IsUnsaved());
}

TEST_CASE("A save closes the open step, so the rest of a drag is a new step", "[editor][history][unsaved]")
{
    Fixture f;
    f.Drag(1, 5);
    f.mHistory.SetUnsaved(false);
    f.Drag(1, 6);

    CHECK(f.mHistory.StepCount() == 2);
    CHECK(f.mHistory.IsUnsaved());

    f.Undo();
    CHECK(f.Current() == 5);
    CHECK_FALSE(f.mHistory.IsUnsaved());
}

TEST_CASE("Once the saved step is dropped from history, the marker stays until the next save",
    "[editor][history][unsaved]")
{
    Fixture f(2500);
    const auto submit = [&f](int value)
        {
            f.mHistory.Submit(std::make_unique<SetValue>(value, kNullEntity, 1000));
            f.Flush();
        };

    SECTION("the saved step trimmed for the budget")
    {
        submit(1);
        f.mHistory.SetUnsaved(false);
        submit(2);
        submit(3);
        REQUIRE(f.mHistory.StepCount() == 2);

        // back to the saved value, but past the trimmed step undo cannot prove it
        f.Undo();
        f.Undo();
        CHECK(f.Current() == 1);
    }
    SECTION("saved with no steps, then the first one trimmed")
    {
        f.mHistory.SetUnsaved(false);
        submit(1);
        submit(2);
        submit(3);
        REQUIRE(f.mHistory.StepCount() == 2);

        f.Undo();
        f.Undo();
    }
    SECTION("the saved step dropped as a redo step")
    {
        f.Set(1);
        f.Set(2);
        f.mHistory.SetUnsaved(false);
        f.Undo();
        f.Set(3);

        f.Undo();
        CHECK(f.Current() == 1);
    }

    CHECK(f.mHistory.IsUnsaved());

    f.mHistory.SetUnsaved(false);
    CHECK_FALSE(f.mHistory.IsUnsaved());
}

TEST_CASE("Clearing history keeps the marker as it was", "[editor][history][unsaved]")
{
    Fixture f;

    SECTION("saved")
    {
        f.Set(1);
        f.mHistory.SetUnsaved(false);
        f.mHistory.Clear();
        CHECK_FALSE(f.mHistory.IsUnsaved());

        f.Set(2);
        CHECK(f.mHistory.IsUnsaved());
    }
    SECTION("unsaved")
    {
        f.Set(1);
        f.mHistory.Clear();
        CHECK(f.mHistory.IsUnsaved());

        f.Set(2);
        f.Undo();
        CHECK(f.mHistory.IsUnsaved());
    }
}

TEST_CASE("Setting the marker by hand", "[editor][history][unsaved]")
{
    Fixture f;
    f.Set(1);

    SECTION("off, as New and Load do after clearing")
    {
        f.mHistory.Clear();
        f.mHistory.SetUnsaved(false);
        CHECK_FALSE(f.mHistory.IsUnsaved());
    }
    SECTION("on, as Stop does for a scene unsaved before Play")
    {
        f.mHistory.SetUnsaved(false);
        f.mHistory.Clear();
        f.mHistory.SetUnsaved(true);
        CHECK(f.mHistory.IsUnsaved());

        f.Set(2);
        f.Undo();
        CHECK(f.mHistory.IsUnsaved());
    }
}
