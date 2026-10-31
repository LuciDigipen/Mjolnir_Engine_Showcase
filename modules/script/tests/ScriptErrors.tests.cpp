/**
 * @file ScriptErrors.tests.cpp
 * @author Dong Ta
 * @brief Tests for what a script that errors every frame leaves in the log.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/World.h>
#include <core/log/Log.h>
#include <core/log/LogHistory.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace
{
    constexpr std::string_view kBroken = R"LUA(
local T = {}

function T.OnUpdate(self, world, entity, dt)
    local missing = nil
    return missing.field
end

return T
)LUA";
}

TEST_CASE("A script that errors every frame leaves one counted entry in the log", "[script][host][log]")
{
    mir::InitLog({ .filePath = (std::filesystem::temp_directory_path() / "mir_script_errors.log").string(),
        .consoleLevel = mir::LogLevel::Off });
    mir::LogHistory::Clear();

    mir::World world;
    mir::CommandBuffer commands;
    mir::ScriptHost host;
    REQUIRE(host.LoadScriptSource("broken", kBroken));
    const mir::Entity entity = world.CreateEntity();
    const int32_t instance = host.CreateInstance("broken");
    REQUIRE(instance != -1);

    for (int frame = 0; frame < 300; ++frame)
        host.CallOnUpdate(world, commands, entity, instance, 1.0f / 60.0f);
    MIR_LOG_INFO("after the frames");

    const auto entries = mir::LogHistory::Snapshot();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].level == mir::LogLevel::Error);
    CHECK(entries[0].message.find("OnUpdate failed") != std::string::npos);
    CHECK(entries[0].count == 300);
    CHECK(entries[1].message == "after the frames");

    host.DestroyInstance(instance);
    mir::ShutdownLog();
}
