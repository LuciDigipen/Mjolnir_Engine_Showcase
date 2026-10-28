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
#include <editor/history/AuthoredEdit.h>
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

TEST_CASE("Discrete reference edits cannot merge with neighboring edits", "[editor][history]")
{
    Editing e;
    const Sample s = BuildSample(e);
    const Entity original = TargetOf(e.mWorld, s.b);
    auto edit = [&](Entity target)
    {
        return mir::MakeSetFieldsEditCommand(s.b, RefOps(),
            { ValueOf(RefOps(), "target", target) }, "", nullptr);
    };

    e.mHistory.SetUnsaved(false);
    e.mHistory.Submit(edit(s.x));
    e.mHistory.Submit(edit(s.z), true);
    e.mHistory.Submit(edit(kNullEntity), true);
    e.mHistory.Submit(edit(s.w));
    e.Flush();
    REQUIRE(e.mHistory.StepCount() == 4);
    CHECK(e.mHistory.IsUnsaved());
    CHECK(TargetOf(e.mWorld, s.b) == s.w);
    e.Undo();
    CHECK(TargetOf(e.mWorld, s.b) == kNullEntity);
    e.Undo();
    CHECK(TargetOf(e.mWorld, s.b) == s.z);
    e.Undo();
    CHECK(TargetOf(e.mWorld, s.b) == s.x);
    e.Undo();
    CHECK(TargetOf(e.mWorld, s.b) == original);
    CHECK_FALSE(e.mHistory.IsUnsaved());
    for (int i = 0; i < 4; ++i)
        e.Redo();
    CHECK(TargetOf(e.mWorld, s.b) == s.w);

    e.mHistory.Submit(edit(s.w), true);
    e.Flush();
    CHECK(e.mHistory.StepCount() == 4);
}

TEST_CASE("Reference edits save and reload stable targets across runtime handles", "[editor][history][entity-ref]")
{
    Editing e;
    const Sample s = BuildSample(e);
    Entity target = s.x;
    SECTION("assign") {}
    SECTION("replace") { target = s.z; }
    SECTION("clear") { target = kNullEntity; }
    const Files before = e.Saved();
    e.mHistory.Submit(mir::MakeSetFieldsEditCommand(s.b, RefOps(),
        { ValueOf(RefOps(), "target", target) }, "", nullptr), true);
    e.Flush();
    const Files after = e.Saved();
    e.Undo();
    CHECK(Differences(e.Saved(), before).empty());
    e.Redo();
    CHECK(Differences(e.Saved(), after).empty());

    const auto dir = std::filesystem::temp_directory_path() / "mir_editor_tests" /
        ("ref_reload_" + std::to_string(std::random_device{}()));
    struct Cleanup
    {
        std::filesystem::path mPath;
        ~Cleanup() { std::filesystem::remove_all(mPath); }
    } cleanup{ dir };
    REQUIRE(mir::SaveScene(e.mWorld, dir, e.mScene));
    World loadedWorld;
    // Occupy the old slot range so a runtime handle cannot accidentally pass.
    for (int i = 0; i < 16; ++i)
        loadedWorld.CreateEntity();
    const LoadedScene loaded = mir::LoadScene(loadedWorld, dir);
    const Entity loadedReferrer = loaded.mEntities.at(e.IdOf(s.b));
    const Entity resolved = TargetOf(loadedWorld, loadedReferrer);
    if (target.IsNull())
        CHECK(resolved == kNullEntity);
    else
    {
        CHECK(resolved == loaded.mEntities.at(e.IdOf(target)));
        CHECK(resolved != target);
        CHECK(loadedWorld.IsAlive(resolved));
    }
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
