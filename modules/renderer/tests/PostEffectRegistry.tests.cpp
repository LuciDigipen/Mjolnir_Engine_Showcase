/**
 * @file PostEffectRegistry.tests.cpp
 * @author Rahul Nair
 * @brief Tests for the Post-Processing panel's name -> params-UI registry.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/PostEffectRegistry.h>

#include <catch2/catch_test_macros.hpp>

namespace
{
    using mir::PostEffectContext;
    using mir::PostEffectDesc;
    using mir::PostEffectRegistry;

    bool gDrawParamsCalled = false;

    void DrawRegistryProbeParams(nlohmann::json &params, const PostEffectContext &)
    {
        gDrawParamsCalled = true;
        params["touched"] = true;
    }
}

TEST_CASE("PostEffectRegistry: registered effects appear in All(), in registration order", "[renderer]")
{
    PostEffectRegistry::Instance().Register("PostEffectRegistryTestProbeA");
    PostEffectRegistry::Instance().Register("PostEffectRegistryTestProbeB", &DrawRegistryProbeParams);

    const PostEffectDesc *probeA = nullptr;
    const PostEffectDesc *probeB = nullptr;
    size_t indexA = 0;
    size_t indexB = 0;
    size_t index = 0;
    for (const PostEffectDesc &effect : PostEffectRegistry::Instance().All())
    {
        if (effect.mName == "PostEffectRegistryTestProbeA")
        {
            probeA = &effect;
            indexA = index;
        }
        if (effect.mName == "PostEffectRegistryTestProbeB")
        {
            probeB = &effect;
            indexB = index;
        }
        ++index;
    }

    REQUIRE(probeA != nullptr);
    REQUIRE(probeB != nullptr);
    CHECK(probeA->mDrawParams == nullptr);
    CHECK(indexA < indexB);

    REQUIRE(probeB->mDrawParams != nullptr);
    nlohmann::json params = nlohmann::json::object();
    probeB->mDrawParams(params, PostEffectContext{});
    CHECK(gDrawParamsCalled);
    CHECK(params.value("touched", false));
}
