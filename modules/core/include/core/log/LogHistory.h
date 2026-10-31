/**
 * @file LogHistory.h
 * @author Rahul Nair
 * @brief In-memory ring buffer of recent log entries, for UI consumption.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "core/log/Log.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mir
{
    struct LogEntry
    {
        LogLevel level = LogLevel::Info;
        std::string message;
        std::string file;
        int line = 0;
        std::chrono::system_clock::time_point time;
        // occurrences collapsed into this entry, the first included
        uint64_t count = 1;
    };

    class LogHistory
    {
    public:
        LogHistory() = delete;

        static void SetCapacity(std::size_t capacity);
        // the id names this entry for AddRepeat
        static uint64_t Push(LogEntry entry);
        // false once the entry is evicted or cleared
        static bool AddRepeat(uint64_t id);
        static std::vector<LogEntry> Snapshot();
        static void Clear();
    };
}
