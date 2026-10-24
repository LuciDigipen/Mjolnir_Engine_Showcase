/**
 * @file ErrorReport.h
 * @author sumin.park
 * @brief Hands a fatal SetupError through reporter box, exit codes, and last-error.json.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/log/SetupError.h>

#include <filesystem>

namespace mir
{
    inline constexpr std::string_view kErrorReportFileName = "last-error.json";

    // LogDir()/last-error.json
    std::filesystem::path ErrorReportPath();

    // Removes an earlier run's report, so a report on disk always belongs to the latest run
    void ClearErrorReport();

    // Logs the error, writes last-error.json, starts the reporter box and returns the exit code.
    // Call before tearing anything down, so a failure during teardown cannot lose the report.
    int ReportFatal(const SetupError &error);
}// namespace mir
