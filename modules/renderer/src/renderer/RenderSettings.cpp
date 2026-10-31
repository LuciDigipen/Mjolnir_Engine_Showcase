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

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace mir
{
    namespace
    {
        using json = nlohmann::json;

        template <typename Value>
        bool ReadNumberValue(const json &node, Value &value)
        {
            if (!node.is_number())
                return false;
            const double number = node.get<double>();
            if (!std::isfinite(number) || number < std::numeric_limits<Value>::lowest() ||
                number > std::numeric_limits<Value>::max() ||
                (std::is_integral_v<Value> && std::trunc(number) != number))
                return false;
            value = static_cast<Value>(number);
            return true;
        }

        template <typename Value>
        bool ReadNumber(const json &object, const char *key, Value &value)
        {
            const auto it = object.find(key);
            return it == object.end() || ReadNumberValue(*it, value);
        }

        bool ReadBool(const json &object, const char *key, bool &value)
        {
            const auto it = object.find(key);
            if (it == object.end())
                return true;
            if (!it->is_boolean())
                return false;
            value = it->get<bool>();
            return true;
        }
    }

    uint32_t SsgiSettings::RaysPerPixel() const
    {
        switch (quality)
        {
        case SsgiQuality::Low: return 1;
        case SsgiQuality::Medium: return 2;
        case SsgiQuality::High: return 4;
        default: return 0;
        }
    }

    uint32_t SsgiSettings::MaxRaySteps() const
    {
        switch (quality)
        {
        case SsgiQuality::Low: return 16;
        case SsgiQuality::Medium: return 32;
        case SsgiQuality::High: return 64;
        default: return 0;
        }
    }

    const char *SsgiSettings::ValidationError() const
    {
        if (RaysPerPixel() == 0)
            return "SSGI quality must be Low, Medium or High.";
        if (!std::isfinite(maxTraceDistance) || maxTraceDistance < 0.01f || maxTraceDistance > 10000.0f)
            return "SSGI trace distance must be between 0.01 and 10000 meters.";
        if (!std::isfinite(thickness) || thickness < 0.001f || thickness > 10.0f || thickness > maxTraceDistance)
            return "SSGI thickness must be between 0.001 and 10 meters and no greater than trace distance.";
        if (!std::isfinite(intensity) || intensity < 0.0f || intensity > 10.0f)
            return "SSGI intensity must be between 0 and 10.";
        return nullptr;
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

    bool RenderSettings::ValidUploadMiBPerFrame(double mib)
    {
        return std::isfinite(mib) && mib > 0.0 && mib <= kMaxUploadMiBPerFrame;
    }

    uint64_t RenderSettings::UploadFrameBytes() const
    {
        // Never zero.
        return std::max<uint64_t>(1, uint64_t(uploadMiBPerFrame * 1024.0 * 1024.0));
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

        // An invalid value keeps the default.
        double uploadMiB = settings.uploadMiBPerFrame;
        if (ReadNumber(*renderingIt, "uploadMiBPerFrame", uploadMiB) && ValidUploadMiBPerFrame(uploadMiB))
            settings.uploadMiBPerFrame = uploadMiB;

        const auto gi = renderingIt->find("ddgi");
        if (gi != renderingIt->end() && gi->is_object())
        {
            // Reject a malformed GI block as a unit, preserving other settings.
            DdgiSettings draft;
            bool valid = true;
            const auto readNumber = [&](const char *key, auto &value)
                {
                    valid = ReadNumber(*gi, key, value) && valid;
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
                        valid = ReadNumberValue((*it)[i], values[i]) && valid;
                };
            valid = ReadBool(*gi, "enabled", draft.enabled) && valid;
            readArray("origin", draft.origin);
            readArray("probeCounts", draft.probeCounts);
            readNumber("probeSpacing", draft.probeSpacing);
            readNumber("raysPerProbe", draft.raysPerProbe);
            readNumber("probesPerFrame", draft.probesPerFrame);
            readNumber("intensity", draft.intensity);
            if (valid && draft.ValidationError() == nullptr)
                settings.ddgi = draft;
        }

        const auto ssgi = renderingIt->find("ssgi");
        if (ssgi != renderingIt->end() && ssgi->is_object())
        {
            SsgiSettings draft;
            bool valid = ReadBool(*ssgi, "enabled", draft.enabled) &&
                ReadBool(*ssgi, "halfResolution", draft.halfResolution) &&
                ReadNumber(*ssgi, "maxTraceDistance", draft.maxTraceDistance) &&
                ReadNumber(*ssgi, "thickness", draft.thickness) &&
                ReadNumber(*ssgi, "intensity", draft.intensity);
            const auto quality = ssgi->find("quality");
            if (quality != ssgi->end())
            {
                if (*quality == "low") draft.quality = SsgiQuality::Low;
                else if (*quality == "medium") draft.quality = SsgiQuality::Medium;
                else if (*quality == "high") draft.quality = SsgiQuality::High;
                else valid = false;
            }
            if (valid && draft.ValidationError() == nullptr)
                settings.ssgi = draft;
        }
        return settings;
    }

    bool RenderSettings::SaveFile(const std::filesystem::path &path) const
    {
        if (ddgi.ValidationError() != nullptr || ssgi.ValidationError() != nullptr ||
            !ValidUploadMiBPerFrame(uploadMiBPerFrame))
            return false;
        return UpdateJsonFile(path, [this](json &root)
            {
                root["rendering"] = json{ { "rayTracing", rayTracing },
                    { "uploadMiBPerFrame", uploadMiBPerFrame },
                    { "ddgi", {
                        { "enabled", ddgi.enabled }, { "origin", ddgi.origin },
                        { "probeCounts", ddgi.probeCounts }, { "probeSpacing", ddgi.probeSpacing },
                        { "raysPerProbe", ddgi.raysPerProbe }, { "probesPerFrame", ddgi.probesPerFrame },
                        { "intensity", ddgi.intensity } } },
                    { "ssgi", {
                        { "enabled", ssgi.enabled },
                        { "quality", ssgi.quality == SsgiQuality::Low ? "low" :
                            ssgi.quality == SsgiQuality::Medium ? "medium" : "high" },
                        { "maxTraceDistance", ssgi.maxTraceDistance }, { "thickness", ssgi.thickness },
                        { "intensity", ssgi.intensity }, { "halfResolution", ssgi.halfResolution } } } };
            });
    }
}
