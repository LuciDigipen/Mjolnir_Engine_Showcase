/**
 * @file CrashHandler.h
 * @author sumin.park
 * @brief Last-resort report for a crash or std::terminate on any thread
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <filesystem>

namespace mir
{
    // Public contract like SetupError's 10-14 and Shutdown's 15; never renumber it
    inline constexpr int kCrashExitCode = 20;

    struct CrashConfig
    {
        // last-error.json, same schema as a caught failure's with kind "crash"
        std::filesystem::path mReportPath;
        std::filesystem::path mDumpPath;
        // empty: no box
        std::filesystem::path mReporterPath;
        std::filesystem::path mLogPath;
    };

    // Once, early in main. Routes access violations, stack overflows, std::terminate,
    // invalid CRT parameters and pure calls to one handler. It writes the report and a
    // minidump from its own thread, starts the reporter and ends the process with
    // kCrashExitCode. Destructors do not run, so nothing else is flushed
    void InstallCrashHandler(const CrashConfig &config);

    // A library may install its own unhandled-exception filter; call once every
    // module is built to take the slot back
    void ReclaimCrashHandler();

    // Call at the top of every thread the engine owns, and from a library's thread-init
    // hook where it has one (Jolt): MSVC keeps the terminate handler per thread, and the
    // thread keeps stack for the handler if it overflows. A thread with no hook (Wwise)
    // still reaches the handler through abort, but two of those terminating at once can
    // lose the race: abort's exit code 3, and no report if the first had not renamed it in
    void ArmCrashHandler();
} // namespace mir
