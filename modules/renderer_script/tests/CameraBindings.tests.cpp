/**
 * @file CameraBindings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the Lua world:get_active_camera, world:set_active_camera and
 *        world:cut_camera bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer_script/CameraBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <renderer/CameraCut.h>
#include <renderer/components/Camera.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string_view>

namespace
{
    using mir::Camera;
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::RuntimeFieldDecl;
    using mir::ScriptHost;
    using mir::World;

    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "other", FieldKind::EntityRef },
            { "noneIsNil", FieldKind::Bool },
            { "setOther", FieldKind::Bool },
            { "getsOther", FieldKind::Bool },
            { "setBare", FieldKind::Bool },
            { "cutSelf", FieldKind::Bool },
            { "cutBare", FieldKind::Bool },
        };
        return ComponentRegistry::Instance().RegisterRuntime("CameraBindingsProbe", fields);
    }

    bool ReadProbeBool(World &world, Entity entity, std::string_view field)
    {
        const ComponentOps &probe = ProbeOps();
        bool value = false;
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    // entity is the script's own entity, a Camera; "other" is a second, inactive Camera.
    constexpr std::string_view kCameraSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local other = world:get(entity, "CameraBindingsProbe").other
    world:set(entity, "CameraBindingsProbe", "noneIsNil", world:get_active_camera() == nil)
    world:set(entity, "CameraBindingsProbe", "setOther", world:set_active_camera(other))
    world:set(entity, "CameraBindingsProbe", "getsOther", world:get_active_camera() == other)

    local bare = world:spawn()
    world:set(entity, "CameraBindingsProbe", "setBare", world:set_active_camera(bare))
end

return T
)LUA";

    constexpr std::string_view kCutSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set(entity, "CameraBindingsProbe", "cutSelf", world:cut_camera(entity))
    world:set(entity, "CameraBindingsProbe", "cutBare", world:cut_camera(world:spawn()))
end

return T
)LUA";
}

TEST_CASE("world:set_active_camera switches the camera at once, and get_active_camera sees it",
    "[script][renderer]")
{
    World world;
    CommandBuffer commands;
    ScriptHost host;
    const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterCameraBindings);

    const Entity self = world.CreateEntity();
    world.AddComponent<Camera>(self, Camera{});
    const Entity other = world.CreateEntity();
    world.AddComponent<Camera>(other, Camera{});

    ProbeOps().AddDefault(world, self);
    ProbeOps().FindField("other")->Write(ProbeOps().GetComponent(world, self), &other);

    REQUIRE(host.LoadScriptSource("active_camera", kCameraSource));
    const int32_t instance = host.CreateInstance("active_camera");
    REQUIRE(instance != -1);

    host.CallOnStart(world, commands, self, instance);
    commands.Flush(world);
    host.DestroyInstance(instance);

    CHECK(ReadProbeBool(world, self, "noneIsNil"));
    CHECK(ReadProbeBool(world, self, "setOther"));
    CHECK(ReadProbeBool(world, self, "getsOther"));
    CHECK_FALSE(ReadProbeBool(world, self, "setBare"));

    CHECK(world.GetComponent<Camera>(other)->mActive);
    CHECK_FALSE(world.GetComponent<Camera>(self)->mActive);
}

TEST_CASE("world:cut_camera marks a cut on a Camera and refuses an entity without one", "[script][renderer]")
{
    World world;
    CommandBuffer commands;
    ScriptHost host;
    const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterCameraBindings);

    const Entity self = world.CreateEntity();
    world.AddComponent<Camera>(self, Camera{});
    ProbeOps().AddDefault(world, self);

    REQUIRE(host.LoadScriptSource("cut_camera", kCutSource));
    const int32_t instance = host.CreateInstance("cut_camera");
    REQUIRE(instance != -1);

    host.CallOnStart(world, commands, self, instance);
    commands.Flush(world);
    host.DestroyInstance(instance);

    CHECK(ReadProbeBool(world, self, "cutSelf"));
    CHECK_FALSE(ReadProbeBool(world, self, "cutBare"));
    CHECK(mir::IsCameraCut(world, self));
}
