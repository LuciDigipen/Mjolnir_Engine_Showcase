/**
 * @file ParticleLifecycle.tests.cpp
 * @author sumin.park
 * @brief Headless emitter ownership, scene persistence and reload transitions.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "ParticleTestWorld.h"

#include <core/ecs/ComponentRegistry.h>
#include <scene/SceneIO.h>

#include <catch2/catch_test_macros.hpp>

#include <cstring>

TEST_CASE("Particle emitters allocate in LateUpdate and stop pause reset have distinct lifetimes", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.Emitter();
    CHECK_FALSE(test.Particles().Find(owner));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    const auto first = test.Particles().Find(owner)->handle;
    REQUIRE(test.Particles().Stop(owner));
    REQUIRE(test.Particles().Pause(owner, true));
    for (int i = 0; i < 10; ++i) test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->phase == mir::ParticlePhase::Draining);
    REQUIRE(test.Particles().Pause(owner, false));
    for (int i = 0; i < 3; ++i) test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
    test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
    REQUIRE(test.Particles().Start(owner));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->handle != first);
    const auto second = test.Particles().Find(owner)->handle;
    REQUIRE(test.Particles().Reset(owner));
    CHECK_FALSE(test.Particles().Runtime().Find(second));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    REQUIRE(test.Particles().Destroy(owner));
    test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
    const auto manual = test.Emitter(false);
    test.Tick();
    CHECK_FALSE(test.Particles().Find(manual));
    test.Particles().Start(manual);
    test.Tick();
    REQUIRE(test.Particles().Find(manual));
}

TEST_CASE("Particle paused worlds freeze drains and removed owners release bounded instances", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.Emitter();
    test.Tick();
    const auto handle = test.Particles().Find(owner)->handle;
    test.Particles().Stop(owner);
    mir::SetPlayState(test.world, mir::PlayState::Paused);
    for (int i = 0; i < 10; ++i) test.Tick();
    REQUIRE(test.Particles().Runtime().Find(handle));
    test.Particles().Pause(owner, true);
    test.world.DestroyEntity(owner);
    REQUIRE(test.Particles().Runtime().Find(handle));
    CHECK(test.Particles().Runtime().Find(handle)->owner == mir::kNullEntity);
    mir::SetPlayState(test.world, mir::PlayState::Running);
    for (int i = 0; i < 3; ++i) test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 0);
}

TEST_CASE("Particle copied components cannot share handles and excess emitters wait for capacity", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto first = test.Emitter();
    test.Tick();
    const auto copy = *test.world.GetComponent<mir::ParticleEmitter>(first);
    const auto second = test.world.CreateEntity();
    mir::AddTransform(test.world, second);
    test.world.AddComponent<mir::ParticleEmitter>(second, copy);
    CHECK(test.world.GetComponent<mir::ParticleEmitter>(second)->instance.IsNull());
    const auto third = test.Emitter();
    test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 2);
    REQUIRE(test.Particles().Find(first));
    const auto firstHandle = test.Particles().Find(first)->handle;
    test.Particles().Destroy(first);
    for (int i = 0; i < 3; ++i) test.Tick();
    REQUIRE(test.Particles().Find(second));
    REQUIRE(test.Particles().Find(third));
    CHECK(test.Particles().Find(second)->handle != firstHandle);
    CHECK(test.Particles().Find(third)->handle != firstHandle);
}

TEST_CASE("Particle cleared effect references release instances and component removal drains", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.Emitter();
    test.Tick();
    const auto first = test.Particles().Find(owner)->handle;
    auto &emitter = *test.world.GetComponent<mir::ParticleEmitter>(owner);
    emitter.effectName[0] = '\0';
    test.Tick();
    CHECK_FALSE(test.Particles().Runtime().Find(first));
    CHECK(test.Particles().Runtime().Stats().active == 0);
    std::strcpy(emitter.effectName, "effects/exhaust.particle.json");
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    const auto second = test.Particles().Find(owner)->handle;
    test.world.RemoveComponent<mir::ParticleEmitter>(owner);
    REQUIRE(test.Particles().Runtime().Find(second));
    CHECK(test.Particles().Runtime().Find(second)->owner.IsNull());
    for (int i = 0; i < 3; ++i) test.Tick();
    CHECK(test.Particles().Runtime().Stats().active == 0);
}

TEST_CASE("Particle reload stages preserve old state on abort and reset bindings on commit", "[particles]")
{
    mir::test::ParticleTestWorld test;
    const auto owner = test.Emitter();
    test.Tick();
    const auto old = test.Particles().Find(owner)->handle;

    test.files.Publish(0.1f, false);
    test.assets.BeginReload();
    CHECK_THROWS(test.StageEvent(mir::Stage::ReloadPrepare));
    test.StageEvent(mir::Stage::ReloadAbort);
    test.assets.AbortReload();
    REQUIRE(test.Particles().Runtime().Find(old));
    CHECK(test.Particles().Find(owner)->effect.lifetime == 0.05f);

    test.files.Publish(0.1f);
    test.assets.BeginReload();
    REQUIRE_NOTHROW(test.StageEvent(mir::Stage::ReloadPrepare));
    REQUIRE(test.Particles().Runtime().Find(old));
    test.Particles().Pause(owner, true);
    test.assets.CommitReload();
    test.StageEvent(mir::Stage::ReloadCommit);
    CHECK_FALSE(test.Particles().Runtime().Find(old));
    test.Tick();
    REQUIRE(test.Particles().Find(owner));
    CHECK(test.Particles().Find(owner)->paused);
    CHECK(test.Particles().Find(owner)->effect.lifetime == 0.1f);
    test.Particles().Stop(owner);
    test.files.Publish(0.2f);
    test.assets.BeginReload();
    test.StageEvent(mir::Stage::ReloadPrepare);
    test.assets.CommitReload();
    test.StageEvent(mir::Stage::ReloadCommit);
    test.Tick();
    CHECK_FALSE(test.Particles().Find(owner));
}

TEST_CASE("Particle save load and repeated play scene transitions never serialize live handles", "[particles]")
{
    mir::test::ParticleTestWorld test;
    auto scene = mir::NewScene("particle lifecycle");
    auto owner = test.Emitter();
    scene.mEntities.emplace(mir::AllocateStableId(scene), owner);
    const auto dir = test.files.Root() / "scene";
    test.Tick();
    const auto old = test.Particles().Find(owner)->handle;
    REQUIRE(mir::SaveScene(test.world, dir, scene));
    mir::UnloadScene(test.world, scene);
    test.StageEvent(mir::Stage::SceneUnload);
    CHECK(test.Particles().Runtime().Stats().active == 0);
    CHECK_FALSE(test.Particles().Runtime().Find(old));
    scene = mir::LoadScene(test.world, dir);
    REQUIRE(scene.mEntities.size() == 1);
    owner = scene.mEntities.begin()->second;
    test.StageEvent(mir::Stage::SceneLoad);
    const auto *emitter = test.world.GetComponent<mir::ParticleEmitter>(owner);
    REQUIRE(emitter);
    CHECK(emitter->instance.IsNull());
    CHECK(std::string(emitter->effectName) == "effects/exhaust.particle.json");

    const auto *ops = mir::ComponentRegistry::Instance().Find("ParticleEmitter");
    REQUIRE(ops);
    CHECK(ops->mFields.size() == 2);
    CHECK(ops->FindField("instance") == nullptr);
    for (int i = 0; i < 10; ++i)
    {
        test.StageEvent(mir::Stage::PlayBegin);
        test.Tick();
        REQUIRE(test.Particles().Find(owner));
        CHECK(test.Particles().Runtime().Stats().active == 1);
        test.StageEvent(mir::Stage::PlayEnd);
        CHECK(test.Particles().Runtime().Stats().active == 0);
        CHECK(test.world.GetComponent<mir::ParticleEmitter>(owner)->instance.IsNull());
    }
}
