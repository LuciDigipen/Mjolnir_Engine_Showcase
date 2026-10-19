/**
 * @file PostProcessingBindings.tests.cpp
 * @author Rahul Nair
 * @brief Tests for the Lua world:set_fog/get_fog, set_hdr/get_hdr and
 *        set_bloom/get_bloom bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <script/ScriptHost.h>

#include <core/ecs/CommandBuffer.h>
#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <renderer/BloomEffect.h>
#include <renderer/FogEffect.h>
#include <renderer/HdrEffect.h>
#include <renderer/PostProcessingSettings.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
    using mir::BloomParams;
    using mir::CommandBuffer;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::FogParams;
    using mir::HdrParams;
    using mir::PostProcessingSettings;
    using mir::ReadBloomParams;
    using mir::ReadFogParams;
    using mir::ReadHdrParams;
    using mir::RuntimeFieldDecl;
    using mir::ScriptHost;
    using mir::World;

    const ComponentOps &ProbeOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "setOk", FieldKind::Bool },
            { "bloomEnabled", FieldKind::Bool },
            { "bloomThreshold", FieldKind::Float },
            { "bloomIntensity", FieldKind::Float },
            { "bloomRadius", FieldKind::Float },
            { "fogDensity", FieldKind::Float },
            { "fogStart", FieldKind::Float },
            { "fogEnd", FieldKind::Float },
            { "fogColorX", FieldKind::Float },
            { "hdrExposure", FieldKind::Float },
            { "hdrOperatorIsAces", FieldKind::Bool },
            { "reached", FieldKind::Bool },
        };
        return ComponentRegistry::Instance().RegisterRuntime("PostProcessingBindingsProbe", fields);
    }

    template <typename T>
    T ReadProbeField(World &world, Entity entity, std::string_view field)
    {
        const ComponentOps &probe = ProbeOps();
        T value{};
        probe.FindField(field)->Read(probe.GetComponent(world, entity), &value);
        return value;
    }

    struct Fixture
    {
        World world;
        CommandBuffer commands;
        ScriptHost host;
        Entity entity{};
        int32_t instance = -1;

        explicit Fixture(std::string_view source, std::string_view name, bool emplaceSettings = true)
        {
            if (emplaceSettings)
                world.EmplaceResource<PostProcessingSettings>();

            entity = world.CreateEntity();
            ProbeOps().AddDefault(world, entity);

            REQUIRE(host.LoadScriptSource(std::string(name), source));
            instance = host.CreateInstance(std::string(name));
            REQUIRE(instance != -1);
        }

        ~Fixture() { host.DestroyInstance(instance); }

        void Start()
        {
            host.CallOnStart(world, commands, entity, instance);
            commands.Flush(world);
        }
    };

    constexpr std::string_view kSetAndGetEverything = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local ok = world:set_bloom({ enabled = true, threshold = 1.25, intensity = 0.75, radius = 2.0 })
        and world:set_fog({ enabled = true, color = { x = 0.1, y = 0.2, z = 0.3 },
                             density = 0.02, startDistance = 5.0, endDistance = 50.0 })
        and world:set_hdr({ enabled = true, exposure = 1.5, tonemapOperator = "aces" })

    local bloom = world:get_bloom()
    local fog = world:get_fog()
    local hdr = world:get_hdr()

    world:set(entity, "PostProcessingBindingsProbe", "setOk", ok)
    world:set(entity, "PostProcessingBindingsProbe", "bloomEnabled", bloom.enabled)
    world:set(entity, "PostProcessingBindingsProbe", "bloomThreshold", bloom.threshold)
    world:set(entity, "PostProcessingBindingsProbe", "bloomIntensity", bloom.intensity)
    world:set(entity, "PostProcessingBindingsProbe", "bloomRadius", bloom.radius)
    world:set(entity, "PostProcessingBindingsProbe", "fogDensity", fog.density)
    world:set(entity, "PostProcessingBindingsProbe", "fogStart", fog.startDistance)
    world:set(entity, "PostProcessingBindingsProbe", "fogEnd", fog.endDistance)
    world:set(entity, "PostProcessingBindingsProbe", "fogColorX", fog.color.x)
    world:set(entity, "PostProcessingBindingsProbe", "hdrExposure", hdr.exposure)
    world:set(entity, "PostProcessingBindingsProbe", "hdrOperatorIsAces", hdr.tonemapOperator == "aces")
    world:set(entity, "PostProcessingBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kPartialUpdatePreservesRest = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set_bloom({ enabled = true, threshold = 1.25, intensity = 0.75, radius = 2.0 })
    -- only intensity changes; threshold and radius must survive untouched
    world:set_bloom({ intensity = 0.1 })

    local bloom = world:get_bloom()
    world:set(entity, "PostProcessingBindingsProbe", "bloomThreshold", bloom.threshold)
    world:set(entity, "PostProcessingBindingsProbe", "bloomIntensity", bloom.intensity)
    world:set(entity, "PostProcessingBindingsProbe", "bloomRadius", bloom.radius)
    world:set(entity, "PostProcessingBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kMalformedRefusesWhole = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    world:set_bloom({ enabled = true, threshold = 1.25, intensity = 0.75, radius = 2.0 })

    local anySucceeded = world:set_bloom({ threshold = "bright" })
        or world:set_fog({ color = "red" })
        or world:set_hdr({ tonemapOperator = "nonsense" })

    local bloom = world:get_bloom()
    world:set(entity, "PostProcessingBindingsProbe", "setOk", not anySucceeded)
    world:set(entity, "PostProcessingBindingsProbe", "bloomThreshold", bloom.threshold)
    world:set(entity, "PostProcessingBindingsProbe", "reached", true)
end

return T
)LUA";

    constexpr std::string_view kNoResourceIsRefusedNotCrashed = R"LUA(
local T = {}

function T.NewInstanceData()
    return {}
end

function T.OnStart(self, world, entity)
    local setFailed = not world:set_bloom({ enabled = true })
    local getIsNil = world:get_bloom() == nil

    world:set(entity, "PostProcessingBindingsProbe", "setOk", setFailed and getIsNil)
    world:set(entity, "PostProcessingBindingsProbe", "reached", true)
end

return T
)LUA";
}

TEST_CASE("set_bloom/set_fog/set_hdr write PostProcessingSettings and get_* reads it back",
    "[script][postprocessing]")
{
    Fixture fixture(kSetAndGetEverything, "postfx_set_and_get");
    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "setOk"));

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "bloomEnabled"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomThreshold") == 1.25f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomIntensity") == 0.75f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomRadius") == 2.0f);

    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "fogDensity") == 0.02f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "fogStart") == 5.0f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "fogEnd") == 50.0f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "fogColorX") == 0.1f);

    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "hdrExposure") == 1.5f);
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "hdrOperatorIsAces"));

    // Also verify straight from the resource - the Lua round-trip isn't the only witness.
    const PostProcessingSettings &settings = fixture.world.Resource<PostProcessingSettings>();
    const BloomParams bloom = ReadBloomParams(settings.effects.at("Bloom").params);
    CHECK(bloom.threshold == 1.25f);
    const FogParams fog = ReadFogParams(settings.effects.at("Fog").params);
    CHECK(fog.end == 50.0f);
    const HdrParams hdr = ReadHdrParams(settings.effects.at("HDR").params);
    CHECK(hdr.exposure == 1.5f);
}

TEST_CASE("a partial set_bloom table only touches the fields it names", "[script][postprocessing]")
{
    Fixture fixture(kPartialUpdatePreservesRest, "postfx_partial_update");
    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomThreshold") == 1.25f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomIntensity") == 0.1f);
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomRadius") == 2.0f);
}

TEST_CASE("a wrong-shaped field refuses the whole set_* call, not just that field",
    "[script][postprocessing]")
{
    Fixture fixture(kMalformedRefusesWhole, "postfx_malformed");
    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "setOk"));
    // The bad call wrote nothing: the earlier, valid threshold survives.
    CHECK(ReadProbeField<float>(fixture.world, fixture.entity, "bloomThreshold") == 1.25f);
}

TEST_CASE("the bindings refuse rather than crash when a world has no PostProcessingSettings",
    "[script][postprocessing]")
{
    Fixture fixture(kNoResourceIsRefusedNotCrashed, "postfx_no_resource", /*emplaceSettings*/ false);
    fixture.Start();

    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "reached"));
    CHECK(ReadProbeField<bool>(fixture.world, fixture.entity, "setOk"));
}
