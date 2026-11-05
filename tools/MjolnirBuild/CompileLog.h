/**
 * @file CompileLog.h
 * @author Quy Dong Ta
 * @brief Reads mjolnir_game_build.log: the phase the compile is in, Ninja's progress, and the first errors.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace mir::build
{
    // mjolnir_game_build.bat echoes this and the phase's name as each phase starts
    inline constexpr std::string_view kPhaseMarker = "mjolnir_game_build: ";

    struct CompileProgress
    {
        // environment, configure, compile or install; empty before the first
        std::string mPhase;
        // Ninja's [done/total] in the compile phase; both 0 until it prints one
        std::size_t mDone = 0;
        std::size_t mTotal = 0;

        bool operator==(const CompileProgress &) const = default;
    };

    // From complete lines only, since the log may be mid-write
    CompileProgress LogProgress(std::string_view log);

    // The first max distinct lines naming an error: error C..., error LNK..., [ERROR:...] from vcvars, or CMake Error
    // with its next line
    std::vector<std::string> LogErrors(std::string_view log, std::size_t max = 3);
}
