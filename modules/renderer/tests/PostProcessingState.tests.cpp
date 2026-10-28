/**
 * @file PostProcessingState.tests.cpp
 * @author Rahul Nair
 * @brief Tests for renderer/PostProcessingState.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <renderer/PostProcessingState.h>

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <string>

namespace
{
    void SetEffect(mir::PostProcessingSettings &settings, std::string_view name, bool enabled,
        nlohmann::json params = nlohmann::json::object())
    {
        settings.effects[std::string(name)] = mir::PostEffectState{ enabled, std::move(params) };
    }
}

TEST_CASE("PostProcessingState: an empty settings resource resolves every effect to neutral", "[renderer]")
{
    const mir::PostProcessingState state = mir::ResolvePostProcessing({}, mir::Camera{ .mExposure = 3.0f }, true, true);

    CHECK(state.fog.density == 0.0f);
    CHECK(state.exposure == 1.0f);
    CHECK(state.hdr.op == mir::TonemapOperator::None);
    CHECK_FALSE(state.bloomEnabled);
    CHECK(state.bloom.intensity == 0.0f);
    CHECK_FALSE(state.depthOfFieldEnabled);
    CHECK_FALSE(state.motionBlurEnabled);
    CHECK(state.antiAliasing.mode == mir::AntiAliasingMode::Off);
    CHECK(state.film.grain == 0.0f);
    CHECK(state.film.contrast == 1.0f);
    CHECK(state.film.saturation == 1.0f);
    CHECK(state.film.vignetteStrength == 0.0f);
}

TEST_CASE("PostProcessingState: enabled effects pass their params through unchanged", "[renderer]")
{
    mir::PostProcessingSettings settings;

    mir::FogParams fog{ { 0.1f, 0.2f, 0.3f }, 0.05f, 2.0f, 50.0f };
    nlohmann::json fogJson = nlohmann::json::object();
    mir::WriteFogParams(fogJson, fog);
    SetEffect(settings, mir::kFogEffectName, true, fogJson);

    const mir::HdrParams hdr{ mir::TonemapOperator::ACES };
    nlohmann::json hdrJson = nlohmann::json::object();
    mir::WriteHdrParams(hdrJson, hdr);
    SetEffect(settings, mir::kHdrEffectName, true, hdrJson);

    const mir::BloomParams bloom{ 0.8f, 0.7f, 1.5f };
    nlohmann::json bloomJson = nlohmann::json::object();
    mir::WriteBloomParams(bloomJson, bloom);
    SetEffect(settings, mir::kBloomEffectName, true, bloomJson);

    const mir::DepthOfFieldParams dof{ 8.0f };
    nlohmann::json dofJson = nlohmann::json::object();
    mir::WriteDepthOfFieldParams(dofJson, dof);
    SetEffect(settings, mir::kDepthOfFieldEffectName, true, dofJson);

    const mir::MotionBlurParams motionBlur{ 12 };
    nlohmann::json motionBlurJson = nlohmann::json::object();
    mir::WriteMotionBlurParams(motionBlurJson, motionBlur);
    SetEffect(settings, mir::kMotionBlurEffectName, true, motionBlurJson);

    const mir::FilmParams film{ 0.2f, 1.1f, 0.9f, 0.4f, 0.8f };
    nlohmann::json filmJson = nlohmann::json::object();
    mir::WriteFilmParams(filmJson, film);
    SetEffect(settings, mir::kFilmEffectName, true, filmJson);

    const mir::Camera lens{ .mExposure = 2.0f, .mFocusDistance = 5.0f, .mAperture = 0.2f, .mShutterStrength = 0.3f };
    const mir::PostProcessingState state = mir::ResolvePostProcessing(settings, lens, false, false);

    CHECK(state.fog.color == fog.color);
    CHECK(state.fog.density == fog.density);
    CHECK(state.fog.start == fog.start);
    CHECK(state.fog.end == fog.end);
    CHECK(state.hdr == hdr);
    CHECK(state.exposure == 2.0f);
    CHECK(state.bloomEnabled);
    CHECK(state.bloom == bloom);
    CHECK(state.depthOfFieldEnabled);
    CHECK(state.depthOfField == dof);
    CHECK(state.focusDistance == 5.0f);
    CHECK(state.aperture == 0.2f);
    CHECK(state.motionBlurEnabled);
    CHECK(state.motionBlur == motionBlur);
    CHECK(state.shutterStrength == 0.3f);
    CHECK(state.film == film);
}

TEST_CASE("PostProcessingState: a disabled HDR ignores the Camera's exposure", "[renderer]")
{
    mir::PostProcessingSettings settings;
    nlohmann::json hdrJson = nlohmann::json::object();
    mir::WriteHdrParams(hdrJson, mir::HdrParams{ mir::TonemapOperator::ACES });
    SetEffect(settings, mir::kHdrEffectName, false, hdrJson);

    const mir::PostProcessingState state =
        mir::ResolvePostProcessing(settings, mir::Camera{ .mExposure = 4.0f }, false, false);

    CHECK(state.exposure == 1.0f);
    CHECK(state.hdr.op == mir::TonemapOperator::None);
}

TEST_CASE("PostProcessingState: negative lens values resolve to 0", "[renderer]")
{
    mir::PostProcessingSettings settings;
    SetEffect(settings, mir::kHdrEffectName, true);

    const mir::Camera lens{ .mExposure = -1.0f, .mFocusDistance = -2.0f, .mAperture = -0.5f,
                            .mShutterStrength = -1.0f };
    const mir::PostProcessingState state = mir::ResolvePostProcessing(settings, lens, false, false);

    CHECK(state.exposure == 0.0f);
    CHECK(state.focusDistance == 0.0f);
    CHECK(state.aperture == 0.0f);
    CHECK(state.shutterStrength == 0.0f);
}

TEST_CASE("PostProcessingState: disabled effects keep their params but neutralize their contribution",
    "[renderer]")
{
    mir::PostProcessingSettings settings;

    nlohmann::json bloomJson = nlohmann::json::object();
    mir::WriteBloomParams(bloomJson, mir::BloomParams{ 0.8f, 0.7f, 1.5f });
    SetEffect(settings, mir::kBloomEffectName, false, bloomJson);

    nlohmann::json filmJson = nlohmann::json::object();
    mir::WriteFilmParams(filmJson, mir::FilmParams{ 0.2f, 1.1f, 0.9f, 0.4f, 0.8f });
    SetEffect(settings, mir::kFilmEffectName, false, filmJson);

    const mir::PostProcessingState state = mir::ResolvePostProcessing(settings, {}, false, false);

    CHECK_FALSE(state.bloomEnabled);
    CHECK(state.bloom.threshold == 0.8f);
    CHECK(state.bloom.intensity == 0.0f);
    CHECK(state.bloom.radius == 1.5f);
    CHECK(state.film == mir::FilmParams{ 0.0f, 1.0f, 1.0f, 0.0f, 0.8f });
}

TEST_CASE("PostProcessingState: motion blur's sample count resolves to at least 1", "[renderer]")
{
    mir::PostProcessingSettings settings;
    nlohmann::json params = nlohmann::json::object();
    mir::WriteMotionBlurParams(params, mir::MotionBlurParams{ 0 });
    SetEffect(settings, mir::kMotionBlurEffectName, true, params);

    CHECK(mir::ResolvePostProcessing(settings, {}, false, false).motionBlur.sampleCount == 1);
}

TEST_CASE("PostProcessingState: a disabled Anti-Aliasing entry resolves to Off", "[renderer]")
{
    mir::PostProcessingSettings settings;
    nlohmann::json params = nlohmann::json::object();
    mir::WriteAntiAliasingParams(params, mir::AntiAliasingParams{ mir::AntiAliasingMode::Fxaa, 4 });
    SetEffect(settings, mir::kAntiAliasingEffectName, false, params);

    CHECK(mir::ResolvePostProcessing(settings, {}, false, false).antiAliasing.mode == mir::AntiAliasingMode::Off);
}

TEST_CASE("PostProcessingState: Anti-Aliasing mode and sample count are clamped", "[renderer]")
{
    mir::PostProcessingSettings settings;
    nlohmann::json params = nlohmann::json::object();
    mir::WriteAntiAliasingParams(params, mir::AntiAliasingParams{ mir::AntiAliasingMode::Dlss, 3 });
    SetEffect(settings, mir::kAntiAliasingEffectName, true, params);

    const mir::PostProcessingState withDlss = mir::ResolvePostProcessing(settings, {}, true, true);
    CHECK(withDlss.antiAliasing.mode == mir::AntiAliasingMode::Dlss);
    CHECK(withDlss.antiAliasing.rtSampleCount == 4);

    CHECK(mir::ResolvePostProcessing(settings, {}, true, false).antiAliasing.mode ==
        mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Dlss, true, false));
    CHECK(mir::ResolvePostProcessing(settings, {}, false, true).antiAliasing.mode ==
        mir::ClampAntiAliasingMode(mir::AntiAliasingMode::Dlss, false, true));
}
