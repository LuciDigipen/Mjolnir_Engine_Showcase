/**
 * @file Process.h
 * @author Jackey Cen
 * @brief Starting another process, finding the ones running from a folder, and showing a file in the file browser.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace mir
{
    // The child dies with this process, however this process ends
    class ChildProcess
    {
    public:
        ChildProcess();
        ~ChildProcess();

        ChildProcess(const ChildProcess &) = delete;
        ChildProcess &operator=(const ChildProcess &) = delete;
        ChildProcess(ChildProcess &&) noexcept;
        ChildProcess &operator=(ChildProcess &&) noexcept;

        // With outputFile, the child's stdout and stderr replace that file's contents (Windows only)
        bool Start(const std::filesystem::path &program, std::span<const std::string> args,
                   const std::filesystem::path &workingDirectory = {}, const std::filesystem::path &outputFile = {});
        bool Running();
        std::optional<int> Poll();
        std::optional<int> Wait();
        bool Terminate();

    private:
        struct Impl;
        std::unique_ptr<Impl> mImpl;
    };

    // Quotes one argument for a Windows command line (CommandLineToArgvW rules)
    std::string QuoteCommandLineArgument(std::string_view arg);

    uint64_t CurrentProcessId();

    // Resident/working-set memory of the running process, in bytes. 0 if unsupported.
    uint64_t CurrentProcessMemoryUsageBytes();

    // Starts program with args in workingDirectory, or the current directory when it is empty. With waitForExit
    // returns its exit code, otherwise 0 once started, one not waited for is left running when this process ends,
    // even inside a job that would kill it, where the job allows. Nullopt (logged) if it cannot start.
    std::optional<int> LaunchProcess(const std::filesystem::path &program, std::span<const std::string> args,
                                     bool waitForExit, const std::filesystem::path &workingDirectory = {});

    struct RunningProcess
    {
        uint32_t mId = 0;
        std::filesystem::path mExecutable;
    };

    // The processes whose executable is in dir or below it, such as a game running from its build folder (Windows only)
    std::vector<RunningProcess> ProcessesRunningFrom(const std::filesystem::path &dir);

    // Opens the file browser at file's folder with file selected, false (logged) when it could not (Windows only)
    bool RevealInFileBrowser(const std::filesystem::path &file);
} // namespace mir
