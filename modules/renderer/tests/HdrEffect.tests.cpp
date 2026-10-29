/**
 * @file HdrEffect.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/HdrEffect.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/HdrEffect.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

TEST_CASE("HdrEffect: ReadHdrParams on an empty params object returns defaults", "[renderer]")
{
    const nlohmann::json params = nlohmann::json::object();
    const mir::HdrParams hdr = mir::ReadHdrParams(params);

    CHECK(hdr.op == mir::HdrParams{}.op);
}

TEST_CASE("HdrEffect: exposure is the Camera's, not part of the HDR params", "[renderer]")
{
    const nlohmann::json params = { { "exposure", 2.5f }, { "tonemapOperator", "aces" } };
    CHECK(mir::ReadHdrParams(params).op == mir::TonemapOperator::ACES);

    nlohmann::json written = nlohmann::json::object();
    mir::WriteHdrParams(written, mir::HdrParams{});
    CHECK_FALSE(written.contains("exposure"));
}

TEST_CASE("HdrEffect: ReadHdrParams falls back to Reinhard for an unknown operator name", "[renderer]")
{
    const nlohmann::json params = { { "tonemapOperator", "not an operator" } };
    const mir::HdrParams hdr = mir::ReadHdrParams(params);

    CHECK(hdr.op == mir::TonemapOperator::Reinhard);
}

TEST_CASE("HdrEffect: WriteHdrParams then ReadHdrParams round-trips every operator", "[renderer]")
{
    for (const mir::TonemapOperator op :
    { mir::TonemapOperator::None, mir::TonemapOperator::Reinhard, mir::TonemapOperator::ACES })
    {
        mir::HdrParams saved;
        saved.op = op;

        nlohmann::json params = nlohmann::json::object();
        mir::WriteHdrParams(params, saved);

        CHECK(mir::ReadHdrParams(params).op == saved.op);
    }
}
