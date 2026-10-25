/**
 * @file RegistrationList.h
 * @author sumin.park
 * @brief Functions run in registration order, each registered through a Registration token
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "core/Registration.h"
#include "core/log/Assert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace mir
{
    // Stage functions, drawers, Lua bindings and screens are all one of these. The
    // list changes only between runs: adding or removing during one could move the
    // function that is running
    template <typename Function>
    class RegistrationList
    {
    public:
        RegistrationList() = default;

        // every token is gone first: one that outlived its list would release into nothing
        ~RegistrationList()
        {
            MIR_ASSERT(mEntries.empty(), "RegistrationList: {} registration(s) outlived their list", mEntries.size());
        }

        RegistrationList(const RegistrationList &) = delete;
        RegistrationList &operator=(const RegistrationList &) = delete;
        RegistrationList(RegistrationList &&) = delete;
        RegistrationList &operator=(RegistrationList &&) = delete;

        [[nodiscard]] Registration Add(Function function)
        {
            MIR_ASSERT(mRunning == 0, "RegistrationList: added to while it runs");
            const uint64_t id = mNextId++;
            mEntries.push_back(Entry{ id, std::move(function) });

            return Registration(mAlive, [this, id]
                {
                    MIR_ASSERT(mRunning == 0, "RegistrationList: removed from while it runs");
                    std::erase_if(mEntries, [id](const Entry &entry) { return entry.id == id; });
                });
        }

        // every function in registration order; a throw stops the run and reaches the caller
        template <typename... Args>
        void Run(Args &&...args)
        {
            const Running running{ mRunning };
            for (const Entry &entry : mEntries)
                entry.function(args...);
        }

        std::size_t Size() const { return mEntries.size(); }

    private:
        struct Entry
        {
            uint64_t id;
            Function function;
        };

        // RAII, so a throwing function still lowers it
        struct Running
        {
            uint32_t &depth;

            explicit Running(uint32_t &value) : depth(value) { ++depth; }
            ~Running() { --depth; }
        };

        std::vector<Entry> mEntries;
        uint64_t mNextId = 0;
        uint32_t mRunning = 0;

        // tokens hold it weakly, to assert the list outlives them. Last, so it expires first
        std::shared_ptr<const int> mAlive = std::make_shared<const int>(0);
    };
}
