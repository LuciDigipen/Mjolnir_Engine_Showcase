/**
 * @file ScriptModule.tests.cpp
 * @author sumin.park
 * @brief Tests for ScriptModule: a removed ScriptRef takes its instance with it.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptModule.h>
#include <script/components/ScriptRef.h>

#include <assets/AssetsModule.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>

namespace
{
    struct Fixture
    {
        mir::World world;
        mir::SystemScheduler scheduler;
        // no manifest there: an empty cache is enough, the test loads its script by hand
        mir::AssetsModule assets{ std::filesystem::temp_directory_path() / "mir_script_module_no_assets" };
        mir::ScriptModule scripts{ { world, scheduler }, assets, "games/Test/assets" };

        Fixture()
        {
            mir::RegisterCoreComponents();
            REQUIRE(scripts.Host().LoadScriptSource("Spin", "return {}"));
        }

        mir::Entity Scripted()
        {
            mir::ScriptRef ref;
            ref.instanceRef = scripts.Host().CreateInstance("Spin");
            REQUIRE(ref.instanceRef >= 0);
            const mir::Entity entity = world.CreateEntity();
            world.AddComponent(entity, ref);
            return entity;
        }
    };
}

TEST_CASE("Removing a ScriptRef destroys its instance", "[script][module]")
{
    Fixture f;
    const mir::Entity entity = f.Scripted();
    REQUIRE(f.scripts.Host().InstanceCount() == 1);

    f.world.RemoveComponent<mir::ScriptRef>(entity);
    CHECK(f.scripts.Host().InstanceCount() == 0);
}

TEST_CASE("Destroying a scripted entity destroys its instance", "[script][module]")
{
    Fixture f;
    f.world.DestroyEntity(f.Scripted());
    CHECK(f.scripts.Host().InstanceCount() == 0);
}
