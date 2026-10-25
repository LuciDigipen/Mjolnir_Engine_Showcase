/**
 * @file CrashProbe.cpp
 * @author sumin.park
 * @brief Child process for CrashHandler.tests.cpp: installs the handler, then crashes as argv[1] says.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/platform/CrashHandler.h>

#include <exception>
#include <filesystem>
#include <latch>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace
{
    // not tail-call shaped, so the optimizer cannot turn it into a loop
    int Recurse(int depth)
    {
        volatile char frame[1024];
        frame[0] = static_cast<char>(depth);
        if (depth < 0)
            return 0;
        return Recurse(depth + 1) + frame[0];
    }

    [[noreturn]] void TerminateWithException(const char *what)
    {
        try
        {
            throw std::runtime_error(what);
        }
        catch (...)
        {
            std::terminate();
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;

    const std::string_view mode = argv[1];
    const std::filesystem::path dir = argv[2];
    mir::InstallCrashHandler({ .mReportPath = dir / "last-error.json",
                               .mDumpPath = dir / "crash.dmp",
                               .mReporterPath = {},
                               .mLogPath = dir / "engine.log" });

    // not armed, like a thread a library owns: reaches the handler through abort
    if (mode == "terminate-worker")
    {
        std::thread([] { TerminateWithException("worker failed"); }).join();
    }
    else if (mode == "terminate-none")
    {
        std::terminate();
    }
    else if (mode == "terminate-two")
    {
        std::latch start(2);
        // armed, as every engine-owned thread is
        std::thread first([&] { mir::ArmCrashHandler(); start.arrive_and_wait(); TerminateWithException("first"); });
        std::thread second([&] { mir::ArmCrashHandler(); start.arrive_and_wait(); TerminateWithException("second"); });
        first.join();
        second.join();
    }
    else if (mode == "access-violation")
    {
        volatile int *address = nullptr;
        *address = 1;
    }
    else if (mode == "stack-overflow")
    {
        return Recurse(0);
    }

    return 3;
}
