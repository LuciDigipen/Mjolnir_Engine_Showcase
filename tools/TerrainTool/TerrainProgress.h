/**
 * @file TerrainProgress.h
 * @author Rahul Nair
 * @brief Stage and step progress for long offline terrain work, printed by the command line.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <string_view>

namespace mir::terrainbake
{
    // One finished line, no newline. Null silences progress, which is the default,
    // so tests and other library callers print nothing. Not thread safe: bakes are serial.
    using TerrainProgressSink = void (*)(std::string_view line);

    // Also restarts the elapsed time.
    void SetTerrainProgressSink(TerrainProgressSink sink);

    void TerrainProgressStage(std::string_view stage);

    // done of total items in a stage. Prints only when the whole percent changes.
    void TerrainProgressStep(std::string_view stage, uint64_t done, uint64_t total);
}
