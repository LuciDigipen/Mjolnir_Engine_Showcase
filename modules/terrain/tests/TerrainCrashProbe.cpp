/**
 * @file TerrainCrashProbe.cpp
 * @author sumin.park
 * @brief Child process for TerrainCrash.tests.cpp: a terrain job runs out of memory.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <core/platform/CrashHandler.h>
#include <terrain/RunGuarded.h>
#include <terrain/TerrainExecutor.h>

#include <chrono>
#include <filesystem>
#include <new>
#include <string>
#include <string_view>
#include <thread>

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;

    const std::filesystem::path dir = argv[1];
    mir::InstallCrashHandler({ .mReportPath = dir / "last-error.json",
                               .mDumpPath = dir / "crash.dmp",
                               .mReporterPath = {},
                               .mLogPath = dir / "engine.log" });

    mir::TerrainWorkerPool pool(1);
    pool.Post([] {
        (void)mir::RunGuarded([]() -> std::string { throw std::bad_alloc(); },
                              [](std::string_view what) { return std::string(what); });
    });

    // the crash handler ends the process before this does
    std::this_thread::sleep_for(std::chrono::seconds(30));
    return 3;
}
