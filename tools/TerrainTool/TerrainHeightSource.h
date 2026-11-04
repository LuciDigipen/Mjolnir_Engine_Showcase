/**
 * @file TerrainHeightSource.h
 * @author Rahul Nair
 * @brief Authored heights behind the "mir.heightmap" generator, read from an MHGT file.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <terrain/TerrainLayout.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace mir::terrainbake
{
    // Little-endian: "MHGT", u32 version, u32 width, u32 height, f64 originX, f64 originZ,
    // f64 spacing, then width * height f32 heights. Row 0 is min Z, column 0 min X.
    inline constexpr uint32_t kHeightSourceVersion = 1;
    inline constexpr size_t kHeightSourceHeaderSize = 40;

    class TerrainHeightSource
    {
    public:
        // Null with error set on anything wrong with the file, its hash, or its fit to the layout.
        static std::shared_ptr<const TerrainHeightSource> Load(const std::filesystem::path &path, uint64_t expectedHash,
            const TerrainLayout &layout, std::string &error);

// Exact on the file's samples, bilinear between; clamps to the file's edge,
// since a coarse source grid filters a ring of samples past the map.
        double Sample(double x, double z) const;

    private:
        TerrainHeightSource() = default;

        uint32_t mWidth = 0;
        uint32_t mHeight = 0;
        double mOriginX = 0.0;
        double mOriginZ = 0.0;
        double mSpacing = 1.0;
        std::vector<float> mHeights;
    };
}
