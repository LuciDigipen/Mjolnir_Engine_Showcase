/**
 * @file FilmEffect.cpp
 * @author Rahul Nair
 * @brief Film look's shape - the single source of truth for its default
 *        params and json layout, shared by VulkanRenderer (apply) and the
 *        Post-Processing panel (editing), so they can't drift apart.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/FilmEffect.h"

#include "renderer/PostEffectRegistry.h"

#include <imgui.h>

namespace mir
{
    namespace
    {
        void DrawFilmParams(nlohmann::json &params, const PostEffectContext &)
        {
            FilmParams film = ReadFilmParams(params);
            bool changed = false;

            changed |= ImGui::DragFloat("Grain", &film.grain, 0.001f, 0.0f, 1.0f, "%.3f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Contrast", &film.contrast, 0.01f, 0.0f, 4.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Saturation", &film.saturation, 0.01f, 0.0f, 4.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Vignette Strength", &film.vignetteStrength, 0.01f, 0.0f, 1.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);
            changed |= ImGui::DragFloat("Vignette Radius", &film.vignetteRadius, 0.01f, 0.1f, 4.0f, "%.2f",
                ImGuiSliderFlags_AlwaysClamp);

            if (changed)
                WriteFilmParams(params, film);
        }
    }

    FilmParams ReadFilmParams(const nlohmann::json &params)
    {
        FilmParams film;
        film.grain = params.value("grain", film.grain);
        film.contrast = params.value("contrast", film.contrast);
        film.saturation = params.value("saturation", film.saturation);
        film.vignetteStrength = params.value("vignetteStrength", film.vignetteStrength);
        film.vignetteRadius = params.value("vignetteRadius", film.vignetteRadius);
        return film;
    }

    void WriteFilmParams(nlohmann::json &params, const FilmParams &film)
    {
        params["grain"] = film.grain;
        params["contrast"] = film.contrast;
        params["saturation"] = film.saturation;
        params["vignetteStrength"] = film.vignetteStrength;
        params["vignetteRadius"] = film.vignetteRadius;
    }

    void RegisterFilmPostEffect()
    {
        PostEffectRegistry::Instance().Register(kFilmEffectName, &DrawFilmParams);
    }
}
