/**
 * @file MeshBindings.tests.cpp
 * @author Quy Dong Ta
 * @brief Tests for the Lua world:mesh_bounds binding.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer_script/MeshBindings.h>
#include <script/BindingApi.h>
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <renderer/components/MeshBounds.h>

#include <catch2/catch_test_macros.hpp>

#include <glm/vec3.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::MeshBounds;
    using mir::RuntimeFieldDecl;
    using mir::ScriptHost;
    using mir::World;

    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "min", FieldKind::Vec3 },
            { "max", FieldKind::Vec3 },
            { "missingIsNil", FieldKind::Bool },
            { "deadIsNil", FieldKind::Bool },
        };
        return ComponentRegistry::Instance().RegisterRuntime("MeshBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbeField(World &world, Entity entity, std::string_view field)
    {
        const ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    constexpr std::string_view kBoundsSource = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local bounds = world:mesh_bounds(entity)
    world:set(entity, "MeshBindingsProbe", "min", bounds.min)
    world:set(entity, "MeshBindingsProbe", "max", bounds.max)

    local bare = world:spawn()
    world:set(entity, "MeshBindingsProbe", "missingIsNil", world:mesh_bounds(bare) == nil)

    world:destroy(bare)
    world:set(entity, "MeshBindingsProbe", "deadIsNil", world:mesh_bounds(bare) == nil)
end

return T
)LUA";
}

TEST_CASE("world:mesh_bounds hands a script the local box, and nil without one", "[script][renderer]")
{
    World world;
    CommandBuffer commands;
    ScriptHost host;
    const mir::Registration bindings = mir::AddBindings(host, &mir::RegisterMeshBindings);

    const Entity entity = world.CreateEntity();
    ProbeOps().AddDefault(world, entity);
    world.AddComponent<MeshBounds>(entity, MeshBounds{ glm::vec3(-1.0f, 0.0f, -2.0f), glm::vec3(1.0f, 3.0f, 2.0f) });

    REQUIRE(host.LoadScriptSource("mesh_bounds", kBoundsSource));
    const int32_t instance = host.CreateInstance("mesh_bounds");
    REQUIRE(instance != -1);

    host.CallOnStart(world, commands, entity, instance);
    commands.Flush(world);
    host.DestroyInstance(instance);

    CHECK(ReadProbeField<glm::vec3>(world, entity, "min") == glm::vec3(-1.0f, 0.0f, -2.0f));
    CHECK(ReadProbeField<glm::vec3>(world, entity, "max") == glm::vec3(1.0f, 3.0f, 2.0f));
    CHECK(ReadProbeField<bool>(world, entity, "missingIsNil"));
    CHECK(ReadProbeField<bool>(world, entity, "deadIsNil"));
}
