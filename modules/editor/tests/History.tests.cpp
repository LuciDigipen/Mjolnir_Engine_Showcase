/**
 * @file History.tests.cpp
 * @author Dong Ta
 * @brief History cursor, budget, selection and id tests, run with a fake command.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/Selection.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
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

        void Undo(CommandContext &context) override { context.mWorld.Resource<Value>().mValue = mBefore; }

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

    class NoSceneOps final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
    };

    struct Fixture
    {
        World mWorld;
        NoSceneOps mScene;
        History mHistory;

        explicit Fixture(std::size_t budget = mir::kHistoryBudget) : mHistory(budget)
        {
            mWorld.EmplaceResource<Value>();
            mWorld.EmplaceResource<Selection>();
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

        void Flush() { mHistory.Flush(mWorld, mScene); }
        int Current() { return mWorld.Resource<Value>().mValue; }
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
