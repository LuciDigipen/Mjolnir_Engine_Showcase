/**
 * @file ComponentCommands.tests.cpp
 * @author Dong Ta
 * @brief Field edits, add component and remove component: do, undo and redo, compared through saved scene files.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "EditingFixture.h"

#include <editor/history/ComponentCommands.h>
#include <editor/history/EntityCommands.h>

#include <core/ecs/TypeId.h>

#include <cstring>
#include <string_view>

namespace
{
    using mir::FieldValue;
    using mir::MakeAddComponentCommand;
    using mir::MakeRemoveComponentCommand;
    using mir::MakeSetFieldsCommand;

    const ComponentOps &TransformOps()
    {
        return *ComponentRegistry::Instance().FindBySeq(mir::TypeIdOf<Transform>().seq);
    }

    template <typename T>
    FieldValue ValueOf(const ComponentOps &ops, std::string_view field, const T &value)
    {
        FieldValue out;
        out.mField = static_cast<uint32_t>(ops.FindField(field) - ops.mFields.data());
        std::memcpy(out.mBytes, &value, sizeof(T));
        return out;
    }

    std::unique_ptr<ICommand> SetScale(Entity entity, float scale)
    {
        return MakeSetFieldsCommand(entity, TransformOps(), { ValueOf(TransformOps(), "scale", glm::vec3(scale)) });
    }

    std::unique_ptr<ICommand> SetTargetCommand(Entity referrer, Entity target)
    {
        return MakeSetFieldsCommand(referrer, RefOps(), { ValueOf(RefOps(), "target", target) });
    }

    glm::vec3 ScaleOf(World &world, Entity entity)
    {
        return world.GetComponent<Transform>(entity)->Scale();
    }
}

TEST_CASE("Setting a field, undo and redo round-trip the scene files", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    const Files before = e.Saved();
    e.Run(SetScale(s.y, 4.0f));
    const Files after = e.Saved();
    CHECK(ScaleOf(e.mWorld, s.y) == glm::vec3(4.0f));

    e.Undo();
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), after) == std::vector<std::string>{});
}

TEST_CASE("One drag is one step, and a release starts the next", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const glm::vec3 original = ScaleOf(e.mWorld, s.y);

    e.Run(SetScale(s.y, 2.0f));
    e.Run(SetScale(s.y, 3.0f));
    e.Run(SetScale(s.y, 4.0f));
    CHECK(e.mHistory.StepCount() == 1);

    e.mHistory.EndStep();
    e.Flush();
    e.Run(SetScale(s.y, 5.0f));
    CHECK(e.mHistory.StepCount() == 2);

    e.Undo();
    CHECK(ScaleOf(e.mWorld, s.y) == glm::vec3(4.0f));

    e.Undo();
    CHECK(ScaleOf(e.mWorld, s.y) == original);
}

TEST_CASE("Pasting every field of a component is one step", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const Transform &copied = *e.mWorld.GetComponent<Transform>(s.b);

    const std::vector<FieldValue> values{ ValueOf(TransformOps(), "position", copied.Position()),
        ValueOf(TransformOps(), "rotation", copied.Rotation()), ValueOf(TransformOps(), "scale", copied.Scale()) };

    const Files before = e.Saved();
    e.Run(MakeSetFieldsCommand(s.y, TransformOps(), values, "Paste Transform Values"));
    const Files pasted = e.Saved();

    CHECK(e.mHistory.StepCount() == 1);
    CHECK(e.mHistory.UndoName() == "Paste Transform Values");
    CHECK(e.mWorld.GetComponent<Transform>(s.y)->Position() == copied.Position());

    e.Undo();
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), pasted) == std::vector<std::string>{});
}

TEST_CASE("Add component, undo and redo round-trip the scene files", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    std::vector<FieldValue> values;
    SECTION("with its defaults") {}
    SECTION("with pasted values") { values.push_back(ValueOf(RefOps(), "target", s.z)); }

    const Files before = e.Saved();
    e.Run(MakeAddComponentCommand(s.x, RefOps(), values));
    const Files added = e.Saved();

    REQUIRE(RefOps().Has(e.mWorld, s.x));
    CHECK(TargetOf(e.mWorld, s.x) == (values.empty() ? kNullEntity : s.z));

    e.Undo();
    CHECK_FALSE(RefOps().Has(e.mWorld, s.x));
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), added) == std::vector<std::string>{});
}

TEST_CASE("Remove component, undo and redo round-trip the scene files", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);

    const Files before = e.Saved();
    e.Run(MakeRemoveComponentCommand(s.y, RefOps()));
    const Files removed = e.Saved();
    CHECK_FALSE(RefOps().Has(e.mWorld, s.y));

    e.Undo();
    REQUIRE(RefOps().Has(e.mWorld, s.y));
    CHECK(TargetOf(e.mWorld, s.y) == s.w);
    CHECK(Differences(e.Saved(), before) == std::vector<std::string>{});

    e.Redo();
    CHECK(Differences(e.Saved(), removed) == std::vector<std::string>{});
}

TEST_CASE("A reference field follows its target through a delete and its undo", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const StableId x = e.IdOf(s.x);

    e.Run(SetTargetCommand(s.b, s.x));
    e.Run(mir::MakeDeleteCommand(s.x));
    e.Undo();
    e.Undo();
    CHECK(TargetOf(e.mWorld, s.b) == s.y);

    e.Redo();
    CHECK(e.Find(x) != s.x);
    CHECK(TargetOf(e.mWorld, s.b) == e.Find(x));
}

TEST_CASE("An edit that cannot run or changes nothing records no step", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const Entity dead = e.Add("dead");
    e.mWorld.DestroyEntity(dead);

    SECTION("a dead entity") { e.Run(SetScale(dead, 2.0f)); }
    SECTION("the value it already has")
    {
        e.Run(MakeSetFieldsCommand(s.y, TransformOps(), { ValueOf(TransformOps(), "scale", ScaleOf(e.mWorld, s.y)) }));
    }
    SECTION("a component it already has") { e.Run(MakeAddComponentCommand(s.y, RefOps())); }
    SECTION("a component it lacks") { e.Run(MakeRemoveComponentCommand(s.x, RefOps())); }

    CHECK(e.mHistory.StepCount() == 0);
}
