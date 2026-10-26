/**
 * @file Sky.h
 * @author Rahul Nair
 * @brief Marks an entity as the scene's sky and carries its Nishita atmosphere parameters
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once
#include "core/ecs/ComponentAsserts.h"
#include "core/ecs/ComponentFields.h"

#include <cfloat>
#include <cstddef>
#include <glm/vec3.hpp>
#include <string_view>

namespace mir
{
    enum class SkyMode : int32_t
    {
        Nishita = 0,
    };

    inline constexpr std::string_view kSkyModeNames[] = { "Nishita" };

    struct Sky
    {
        SkyMode mMode = SkyMode::Nishita;

        float mPlanetRadiusKm = 6371.0f;
        float mAtmosphereRadiusKm = 6471.0f;

        // Physical units are this * 1e-6 (m^-1).
        glm::vec3 mRayleighCoeff{ 5.5f, 13.0f, 22.4f };
        float mMieCoeff = 21.0f;

        float mRayleighScaleHeightKm = 8.0f;
        float mMieScaleHeightKm = 1.2f;

        // [0, 1); how forward-scattering Mie particles are.
        float mMieG = 0.758f;
        float mSunIntensity = 22.0f;
    };

    MIR_ASSERT_COMPONENT(Sky);

    inline constexpr FieldDesc kSkyFields[] = {
        { .mName = "mode", .mKind = FieldKind::Int, .mOffset = offsetof(Sky, mMode),
          .mWidget = FieldWidget::Enum, .mEnumNames = kSkyModeNames },
        { "planetRadiusKm", FieldKind::Float, offsetof(Sky, mPlanetRadiusKm) },
        { "atmosphereRadiusKm", FieldKind::Float, offsetof(Sky, mAtmosphereRadiusKm) },
        { "rayleighCoeff", FieldKind::Vec3, offsetof(Sky, mRayleighCoeff) },
        { .mName = "mieCoeff", .mKind = FieldKind::Float, .mOffset = offsetof(Sky, mMieCoeff),
          .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX },
        { .mName = "rayleighScaleHeightKm", .mKind = FieldKind::Float, .mOffset = offsetof(Sky, mRayleighScaleHeightKm),
          .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX }, // the shader divides by it
        { .mName = "mieScaleHeightKm", .mKind = FieldKind::Float, .mOffset = offsetof(Sky, mMieScaleHeightKm),
          .mClamped = true, .mMin = 0.01f, .mMax = FLT_MAX }, // the shader divides by it
        { .mName = "mieG", .mKind = FieldKind::Float, .mOffset = offsetof(Sky, mMieG),
          .mClamped = true, .mMin = 0.0f, .mMax = 0.999f }, // 1 divides by zero looking at the sun
        { .mName = "sunIntensity", .mKind = FieldKind::Float, .mOffset = offsetof(Sky, mSunIntensity),
          .mClamped = true, .mMin = 0.0f, .mMax = FLT_MAX },
    };
}
