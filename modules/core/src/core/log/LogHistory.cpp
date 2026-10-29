/**
 * @file LogHistory.cpp
 * @author Rahul Nair
 * @brief In-memory ring buffer of recent log entries, for UI consumption.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#include "core/log/LogHistory.h"

#include <deque>
#include <mutex>

namespace mir
{
    namespace
    {
        std::mutex gMutex;
        std::deque<LogEntry> gEntries;
        std::size_t gCapacity = 2000;
        uint64_t gNextId = 0;
    }

    void LogHistory::SetCapacity(std::size_t capacity)
    {
        std::lock_guard lock(gMutex);
        gCapacity = capacity;
        while (gEntries.size() > gCapacity)
            gEntries.pop_front();
    }

    uint64_t LogHistory::Push(LogEntry entry)
    {
        std::lock_guard lock(gMutex);
        const uint64_t id = gNextId++;
        if (gCapacity == 0)
            return id;
        if (gEntries.size() >= gCapacity)
            gEntries.pop_front();
        gEntries.push_back(std::move(entry));
        return id;
    }

    bool LogHistory::AddRepeat(uint64_t id)
    {
        std::lock_guard lock(gMutex);
        if (id >= gNextId || gNextId - id > gEntries.size())
            return false;
        ++gEntries[gEntries.size() - static_cast<std::size_t>(gNextId - id)].count;
        return true;
    }

    std::vector<LogEntry> LogHistory::Snapshot()
    {
        std::lock_guard lock(gMutex);
        return std::vector<LogEntry>(gEntries.begin(), gEntries.end());
    }

    void LogHistory::Clear()
    {
        std::lock_guard lock(gMutex);
        gEntries.clear();
    }
}
