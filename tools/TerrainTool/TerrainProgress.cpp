/**
 * @file TerrainProgress.cpp
 * @author Rahul Nair
 * @brief Stage and step progress for long offline terrain work, printed by the command line.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "TerrainProgress.h"

#include <chrono>
#include <format>
#include <string>

namespace mir::terrainbake
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        TerrainProgressSink gSink = nullptr;
        Clock::time_point gStart;
        std::string gStepStage;
        uint64_t gStepPercent = 0;

        double Elapsed()
        {
            return std::chrono::duration<double>(Clock::now() - gStart).count();
        }
    }

    void SetTerrainProgressSink(TerrainProgressSink sink)
    {
        gSink = sink;
        gStart = Clock::now();
        gStepStage.clear();
    }

    void TerrainProgressStage(std::string_view stage)
    {
        if (gSink)
            gSink(std::format("[{:8.1f}s] {}", Elapsed(), stage));
    }

    void TerrainProgressStep(std::string_view stage, uint64_t done, uint64_t total)
    {
        if (!gSink || total == 0)
            return;
        const uint64_t percent = done >= total ? 100 : done * 100 / total;
        if (stage == gStepStage && percent == gStepPercent)
            return;
        gStepStage = stage;
        gStepPercent = percent;
        gSink(std::format("[{:8.1f}s] {} {}/{} ({}%)", Elapsed(), stage, done, total, percent));
    }
}
