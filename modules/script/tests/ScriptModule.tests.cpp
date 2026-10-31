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

#include <assets/AssetResolver.h>
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
        mir::AssetResolver resolver{ assets, "games/Test/assets" };
        mir::ScriptModule scripts{ { world, scheduler }, assets, resolver };

        Fixture()
        {
            mir::RegisterCoreComponents();
            REQUIRE(scripts.Host().LoadScriptSource("Spin", "return {}"));
        }

        // the instance is attached after the add, the way the resolver does it
        mir::Entity Scripted()
        {
            const mir::Entity entity = world.CreateEntity();
            world.AddComponent(entity, mir::ScriptRef{});
            mir::ScriptRef *ref = world.GetComponent<mir::ScriptRef>(entity);
            ref->instanceRef = scripts.Host().CreateInstance("Spin");
            REQUIRE(ref->instanceRef >= 0);
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

TEST_CASE("A ScriptRef added with another run's instance and started flag starts clean", "[script][module]")
{
    Fixture f;
    mir::ScriptRef copied;
    copied.instanceRef = 7;
    copied.started = true;

    const mir::Entity entity = f.world.CreateEntity();
    f.world.AddComponent(entity, copied);

    CHECK(f.world.GetComponent<mir::ScriptRef>(entity)->instanceRef == -1);
    CHECK_FALSE(f.world.GetComponent<mir::ScriptRef>(entity)->started);
}

TEST_CASE("Destroying a scripted entity destroys its instance", "[script][module]")
{
    Fixture f;
    f.world.DestroyEntity(f.Scripted());
    CHECK(f.scripts.Host().InstanceCount() == 0);
}
