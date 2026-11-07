/**
 * @file SystemOrder.tests.cpp
 * @author Dong Ta
 * @brief Pins the ordering constraints in docs/modules/core.md that this target's modules can see
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <input/InputModule.h>
#include <input/InputSystem.h>
#include <physics/PhysicsModule.h>
#include <physics/PhysicsSystem.h>
#include <physics_layers/PhysicsLayers.h>
#include <script/ScriptFixedSystem.h>
#include <script/ScriptModule.h>
#include <script/ScriptResolveSystem.h>
#include <script/ScriptSystem.h>
#include <window/Window.h>

#include <assets/AssetResolver.h>
#include <assets/AssetsModule.h>
#include <core/ecs/SystemScheduler.h>
#include <core/ecs/TypeId.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <vector>

namespace
{
    class TestWindow final : public mir::Window
    {
    public:
        void PollEvents() override {}
        bool ShouldClose() const override { return false; }
        mir::NativeWindowHandle NativeWindow() const override { return {}; }
        uint32_t Width() const override { return 1; }
        uint32_t Height() const override { return 1; }
    };

    template <typename Earlier, typename Later>
    bool RunsBefore(const mir::SystemScheduler &scheduler, mir::SystemPhase phase)
    {
        const std::vector<std::string_view> names = scheduler.SystemNames(phase);
        const auto earlier = std::ranges::find(names, mir::TrimTypeName<Earlier>());
        const auto later = std::ranges::find(names, mir::TrimTypeName<Later>());
        REQUIRE(earlier != names.end());
        REQUIRE(later != names.end());
        return earlier < later;
    }
}

TEST_CASE("The input, script and physics modules keep the ordering constraints core.md lists", "[system][order]")
{
    const std::filesystem::path none = std::filesystem::temp_directory_path() / "mir_system_order_none";

    TestWindow window;
    mir::World world;
    mir::SystemScheduler scheduler;
    mir::AssetsModule assets{ none };
    world.EmplaceResource<mir::PhysicsLayerTable>();
    mir::InputModule input{ { world, scheduler }, window };
    mir::PhysicsModule physics{ { world, scheduler } };
    mir::AssetResolver resolver{ assets, "games/Test/assets" };
    mir::ScriptModule scripts{ { world, scheduler }, assets, resolver };

    CHECK(RunsBefore<mir::InputSystem, mir::ScriptSystem>(scheduler, mir::SystemPhase::PreUpdate));
    CHECK(RunsBefore<mir::ScriptResolveSystem, mir::ScriptSystem>(scheduler, mir::SystemPhase::PreUpdate));
    CHECK(RunsBefore<mir::ScriptFixedSystem, mir::PhysicsSystem>(scheduler, mir::SystemPhase::FixedUpdate));
}
