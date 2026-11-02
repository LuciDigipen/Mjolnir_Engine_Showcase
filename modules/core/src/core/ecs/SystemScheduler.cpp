/**
 * @file SystemScheduler.cpp
 * @author Dong Ta
 * @brief The tie mode read from the environment, and the Debug log of each phase's order
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "core/ecs/SystemScheduler.h"

#include "core/log/Log.h"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iterator>
#include <random>
#include <string>

namespace mir
{
    namespace
    {
        constexpr std::string_view kPhaseNames[] = {
            "PreUpdate", "Update", "FixedUpdate", "PostUpdate", "Propagate", "LateUpdate", "Render",
        };
        static_assert(std::size(kPhaseNames) == static_cast<std::size_t>(SystemPhase::kCount));

        // test executables never init the log, and the seed must reach their output
        void Announce(const std::string &line)
        {
            if (detail::ShouldLog(LogLevel::Warn))
                MIR_LOG_WARN("{}", line);
            else
                std::fprintf(stderr, "%s\n", line.c_str());
        }

        SystemTies ParseTies(std::string_view text)
        {
            if (text == "reverse")
                return SystemTies{ SystemTies::Reverse };
            if (text == "shuffle")
                return SystemTies{ SystemTies::Shuffle, std::random_device{}() };

            constexpr std::string_view kSeeded = "shuffle:";
            if (text.starts_with(kSeeded))
            {
                const std::string_view digits = text.substr(kSeeded.size());
                uint32_t seed = 0;
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), seed);
                if (error == std::errc{} && end == digits.data() + digits.size())
                    return SystemTies{ SystemTies::Shuffle, seed };
            }

            Announce(std::format("MIR_SYSTEM_TIES='{}' is not reverse, shuffle or shuffle:<seed>; ignored", text));
            return SystemTies{};
        }
    }

    SystemTies SystemTiesFromEnvironment()
    {
        static const SystemTies ties = []
        {
            const char *value = std::getenv("MIR_SYSTEM_TIES");
            if (value == nullptr || *value == '\0')
                return SystemTies{};

            const SystemTies parsed = ParseTies(value);
            if (parsed.mode == SystemTies::Reverse)
                Announce("Systems with equal orders run in reverse registration order (MIR_SYSTEM_TIES=reverse)");
            else if (parsed.mode == SystemTies::Shuffle)
                Announce(std::format("Systems with equal orders run shuffled with seed {}; MIR_SYSTEM_TIES=shuffle:{} repeats it",
                    parsed.seed, parsed.seed));
            return parsed;
        }();
        return ties;
    }

    std::ptrdiff_t SystemScheduler::TieSlot(std::ptrdiff_t ties)
    {
        switch (mTies.mode)
        {
        case SystemTies::Reverse:
            return 0;
        case SystemTies::Shuffle:
        {
            // splitmix64; a uniform slot for every insert makes a uniform permutation
            uint64_t z = (mShuffleState += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            z ^= z >> 31;
            return static_cast<std::ptrdiff_t>(z % static_cast<uint64_t>(ties + 1));
        }
        default:
            return ties;
        }
    }

    void SystemScheduler::LogOrder() const
    {
#if MIR_ACTIVE_LOG_LEVEL <= MIR_LOG_LEVEL_DEBUG
        for (std::size_t phase = 0; phase < kPhaseCount; ++phase)
        {
            if (mPhases[phase].empty())
                continue;

            std::string line;
            for (const Entry &entry : mPhases[phase])
                line += std::format("{}{} {}", line.empty() ? "" : ", ", entry.name, entry.order);
            MIR_LOG_DEBUG("{}: {}", kPhaseNames[phase], line);
        }
#endif
    }
}
