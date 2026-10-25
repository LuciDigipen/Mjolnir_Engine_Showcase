/**
 * @file ModuleUnwind.tests.cpp
 * @author sumin.park
 * @brief The member-order unwind App relies on when a module constructor throws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/ecs/ModuleContext.h>
#include <core/Registration.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <vector>

namespace
{
    std::vector<int> gEvents;

    // stands in for a module: built in member order, and registers a stage on the way
    struct FakeModule
    {
        int id;
        mir::Registration stage;

        FakeModule(mir::ModuleContext context, int moduleId, bool fail) : id(moduleId)
        {
            if (fail)
                throw std::runtime_error("setup failed");
            stage = context.scheduler.AddStage(mir::Stage::SceneLoad, [](mir::SystemContext &) {});
            gEvents.push_back(id);
        }

        ~FakeModule() { gEvents.push_back(-id); }
    };

    // App's shape: World and scheduler first, then the modules in order
    struct FakeApp
    {
        mir::World world;
        mir::SystemScheduler scheduler;
        FakeModule first;
        FakeModule second;
        FakeModule third;

        explicit FakeApp(int failing)
            : first({ world, scheduler }, 1, failing == 1),
              second({ world, scheduler }, 2, failing == 2),
              third({ world, scheduler }, 3, failing == 3)
        {
        }
    };
}

TEST_CASE("A module constructor that throws unwinds the earlier modules in reverse", "[ecs][module]")
{
    gEvents.clear();
    REQUIRE_THROWS_AS(FakeApp(3), std::runtime_error);

    // module 3 never finished, so its destructor never runs; 2 and 1 go in reverse
    CHECK(gEvents == std::vector<int>{ 1, 2, -2, -1 });
}

TEST_CASE("A full build tears down in reverse, every token released first", "[ecs][module]")
{
    gEvents.clear();
    {
        FakeApp app(0);
        CHECK(app.scheduler.FunctionCount(mir::Stage::SceneLoad) == 3);
    }
    CHECK(gEvents == std::vector<int>{ 1, 2, 3, -3, -2, -1 });
}
