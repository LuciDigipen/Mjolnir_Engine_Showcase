/**
 * @file RenderSettings.h
 * @author Rahul Nair
 * @brief Project Settings' "rendering" section. A World resource, loaded and
 *        saved the same way InputMap is (see input/InputMap.h).
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>

namespace mir
{
    enum class SsgiQuality { Low, Medium, High };

    struct SsgiSettings
    {
        bool enabled = false;
        SsgiQuality quality = SsgiQuality::Medium;
        float maxTraceDistance = 10.0f;
        float thickness = 0.1f;
        float intensity = 1.0f;
        bool halfResolution = true;

        bool operator==(const SsgiSettings &) const = default;
        const char *ValidationError() const;
        uint32_t RaysPerPixel() const;
        uint32_t MaxRaySteps() const;
    };

    // One world-anchored, axis-aligned volume. Origin is the grid center in
    // double-precision world meters; spacing is uniform in all three axes.
    struct DdgiSettings
    {
        bool enabled = false;
        std::array<double, 3> origin{ 0.0, 0.0, 0.0 };
        std::array<int, 3> probeCounts{ 8, 8, 8 };
        float probeSpacing = 2.0f;
        int raysPerProbe = 128;
        int probesPerFrame = 512;
        float intensity = 1.0f;

        static constexpr uint64_t kMaxProbes = 32768;
        static constexpr uint64_t kMaxRaysPerFrame = 1048576;
        static constexpr uint64_t kMaxEstimatedBytes = 256ull * 1024 * 1024;

        bool operator==(const DdgiSettings &) const = default;
        // nullptr means valid. Check before using the budget helpers.
        const char *ValidationError() const;
        uint64_t ProbeCount() const;
        uint64_t EstimatedBytes() const;
    };

    struct RenderSettings
    {
        static constexpr double kDefaultUploadMiBPerFrame = 4.0;
        static constexpr double kMaxUploadMiBPerFrame = 1024.0;

        bool rayTracing = false;
        // Streamed mesh bytes the renderer uploads per frame.
        double uploadMiBPerFrame = kDefaultUploadMiBPerFrame;
        DdgiSettings ddgi;
        SsgiSettings ssgi;

        bool operator==(const RenderSettings &) const = default;

        static bool ValidUploadMiBPerFrame(double mib);
        uint64_t UploadFrameBytes() const;

        // nullopt if the file is missing or unreadable; a file with no
        // "rendering" section loads as defaults.
        static std::optional<RenderSettings> LoadFile(const std::filesystem::path &path);
        bool SaveFile(const std::filesystem::path &path) const;
    };
}
