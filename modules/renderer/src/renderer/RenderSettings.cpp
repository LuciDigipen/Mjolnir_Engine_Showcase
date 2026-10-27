/**
 * @file RenderSettings.cpp
 * @author Rahul Nair
 * @brief Project Settings' "rendering" section. A World resource, loaded and
 *        saved the same way InputMap is (see input/InputMap.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "renderer/RenderSettings.h"

#include <serial/JsonFile.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <limits>
#include <type_traits>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;
    }

    uint64_t DdgiSettings::ProbeCount() const
    {
        return uint64_t(probeCounts[0]) * uint64_t(probeCounts[1]) * uint64_t(probeCounts[2]);
    }

    uint64_t DdgiSettings::EstimatedBytes() const
    {
        // Two copies of 16-byte ray/atlas texels: 8x8 irradiance/variability,
        // 16x16 distance and one state texel. Runtime checks actual allocations.
        return 2 * ProbeCount() * (uint64_t(raysPerProbe) + 64 + 64 + 256 + 1) * 16;
    }

    const char *DdgiSettings::ValidationError() const
    {
        for (double axis : origin)
            if (!std::isfinite(axis))
                return "GI volume position must be finite.";
        for (int count : probeCounts)
            if (count < 2 || count > 128)
                return "GI probe counts must be between 2 and 128 per axis.";
        if (!std::isfinite(probeSpacing) || probeSpacing < 0.01f || probeSpacing > 10000.0f)
            return "GI probe spacing must be between 0.01 and 10000 meters.";
        if (raysPerProbe != 64 && raysPerProbe != 128 && raysPerProbe != 256 &&
            raysPerProbe != 512 && raysPerProbe != 1024)
            return "GI rays per probe must be 64, 128, 256, 512 or 1024.";
        if (ProbeCount() > kMaxProbes)
            return "GI volume exceeds 32768 probes.";
        if (probesPerFrame < 1 || uint64_t(probesPerFrame) > ProbeCount())
            return "GI probes per frame must be between 1 and the volume probe count.";
        if (uint64_t(probesPerFrame) * uint64_t(raysPerProbe) > kMaxRaysPerFrame)
            return "GI update exceeds 1048576 rays per frame.";
        if (EstimatedBytes() > kMaxEstimatedBytes)
            return "GI volume exceeds the 256 MiB estimated memory budget.";
        if (!std::isfinite(intensity) || intensity < 0.0f || intensity > 10.0f)
            return "GI intensity must be between 0 and 10.";
        return nullptr;
    }

    std::optional<RenderSettings> RenderSettings::LoadFile(const std::filesystem::path &path)
    {
        const std::optional<json> root = ReadJsonFile(path);
        if (!root || !root->is_object())
            return std::nullopt;

        RenderSettings settings;

        // find() rather than value(): value() hands back the raw node, and a
        // "rendering" that is null or a scalar then throws type_error 306 on the
        // next read. Absent or unusable both mean defaults.
        const auto renderingIt = root->find("rendering");
        if (renderingIt == root->end() || !renderingIt->is_object())
            return settings;

        const auto rt = renderingIt->find("rayTracing");
        if (rt != renderingIt->end() && rt->is_boolean())
            settings.rayTracing = rt->get<bool>();

        const auto gi = renderingIt->find("ddgi");
        if (gi != renderingIt->end() && gi->is_object())
        {
            // Reject a malformed GI block as a unit, preserving other settings.
            DdgiSettings draft;
            bool valid = true;
            const auto readValue = [&](const json &node, auto &value)
                {
                    if (!node.is_number())
                    {
                        valid = false;
                        return;
                    }
                    const double number = node.get<double>();
                    using Value = std::remove_reference_t<decltype(value)>;
                    if (!std::isfinite(number) || number < std::numeric_limits<Value>::lowest() ||
                        number > std::numeric_limits<Value>::max() ||
                        (std::is_integral_v<Value> && std::trunc(number) != number))
                        valid = false;
                    else
                        value = static_cast<Value>(number);
                };
            const auto readNumber = [&](const char *key, auto &value)
                {
                    const auto it = gi->find(key);
                    if (it != gi->end())
                        readValue(*it, value);
                };
            const auto readArray = [&](const char *key, auto &values)
                {
                    const auto it = gi->find(key);
                    if (it == gi->end())
                        return;
                    if (!it->is_array() || it->size() != values.size())
                    {
                        valid = false;
                        return;
                    }
                    for (size_t i = 0; i < values.size(); ++i)
                        readValue((*it)[i], values[i]);
                };
            const auto enabled = gi->find("enabled");
            if (enabled != gi->end())
            {
                if (enabled->is_boolean())
                    draft.enabled = enabled->get<bool>();
                else
                    valid = false;
            }
            readArray("origin", draft.origin);
            readArray("probeCounts", draft.probeCounts);
            readNumber("probeSpacing", draft.probeSpacing);
            readNumber("raysPerProbe", draft.raysPerProbe);
            readNumber("probesPerFrame", draft.probesPerFrame);
            readNumber("intensity", draft.intensity);
            if (valid && draft.ValidationError() == nullptr)
                settings.ddgi = draft;
        }
        return settings;
    }

    bool RenderSettings::SaveFile(const std::filesystem::path &path) const
    {
        if (ddgi.ValidationError() != nullptr)
            return false;
        return UpdateJsonFile(path, [this](json &root)
            {
                root["rendering"] = json{ { "rayTracing", rayTracing },
                    { "ddgi", {
                        { "enabled", ddgi.enabled }, { "origin", ddgi.origin },
                        { "probeCounts", ddgi.probeCounts }, { "probeSpacing", ddgi.probeSpacing },
                        { "raysPerProbe", ddgi.raysPerProbe }, { "probesPerFrame", ddgi.probesPerFrame },
                        { "intensity", ddgi.intensity } } } };
            });
    }
}
