/**
 * @file ErrorReport.cpp
 * @author sumin.park
 * @brief Hands a fatal SetupError through reporter box, exit codes, and last-error.json.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <app/ErrorReport.h>

#include <core/fs/Paths.h>
#include <core/log/Log.h>
#include <core/platform/Process.h>
#include <serial/JsonFile.h>

#include <string>
#include <system_error>
#include <vector>

namespace mir
{
    std::filesystem::path ReporterPath()
    {
        std::filesystem::path path = ExecutablePath();
        const std::filesystem::path extension = path.extension();
        path.replace_filename("MjolnirErrorReporter");
        path += extension;
        return path;
    }

    std::filesystem::path ErrorReportPath()
    {
        return LogDir() / kErrorReportFileName;
    }

    std::filesystem::path CrashDumpPath()
    {
        return LogDir() / kCrashDumpFileName;
    }

    void ClearErrorReport()
    {
        std::error_code ec;
        std::filesystem::remove(ErrorReportPath(), ec);
        std::filesystem::remove(CrashDumpPath(), ec);
    }

    int ReportFatal(const SetupError &error)
    {
        const int exitCode = ExitCodeOf(error.Category());

        MIR_LOG_CRITICAL("Fatal ({}, exit {}): {}", CategoryName(error.Category()), exitCode, error.What());
        if (!error.Path().empty())
            MIR_LOG_CRITICAL("  path:  {}", error.Path().string());
        if (!error.Cause().empty())
            MIR_LOG_CRITICAL("  cause: {}", error.Cause());
        if (!error.Fix().empty())
            MIR_LOG_CRITICAL("  fix:   {}", error.Fix());

        const nlohmann::json report{
            { "kind", "caught" },
            { "category", CategoryName(error.Category()) },
            { "exitCode", exitCode },
            { "what", error.What() },
            { "path", error.Path().generic_string() },
            { "cause", error.Cause() },
            { "fix", error.Fix() },
            { "log", LogFilePath().generic_string() },
        };

        std::error_code ec;
        std::filesystem::create_directories(LogDir(), ec);
        const bool written = WriteJsonFile(ErrorReportPath(), report);

        FlushLog();

        // The box comes from its own process: the engine exits now and never waits on a click
        const std::filesystem::path reporter = ReporterPath();
        if (!written)
        {
            MIR_LOG_ERROR("ReportFatal: no error box, could not write '{}' for the reporter to read; the log and "
                "exit code still report it",
                ErrorReportPath().string());
        }
        else if (!std::filesystem::is_regular_file(reporter, ec))
        {
            MIR_LOG_ERROR("ReportFatal: no error box, '{}' is missing; the log and exit code still report it",
                reporter.string());
        }
        else
        {
            const std::vector<std::string> args{ ErrorReportPath().string() };
            LaunchProcess(reporter, args, false);
        }

        FlushLog();
        return exitCode;
    }
}// namespace mir
