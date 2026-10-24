/**
 * @file main.cpp
 * @author sumin.park
 * @brief Shows the box for a fatal engine error after the engine has already exited.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

// Links no engine module, so whatever broke the engine cannot also stop the box

#include <nlohmann/json.hpp>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <fstream>
#include <string>

namespace
{
    // tinyfd refuses a title or message holding a quote and shows a placeholder instead
    std::string WithoutQuotes(std::string text)
    {
        std::replace(text.begin(), text.end(), '"', '`');
        std::replace(text.begin(), text.end(), '\'', '`');
        return text;
    }

    void AppendField(std::string &message, const nlohmann::json &report, const char *key, const char *label)
    {
        const std::string value = report.value(key, std::string());
        if (value.empty())
            return;
        message += "\n\n";
        message += label;
        message += value;
    }
}

int main(int argc, char **argv)
{
    std::string title = "MjolnirEngine stopped";
    std::string message = "MjolnirEngine stopped with an error. See logs/engine.log next to the engine.";

    if (argc > 1)
    {
        std::ifstream input(argv[1]);
        const nlohmann::json report = nlohmann::json::parse(input, nullptr, false);
        if (report.is_object())
        {
            title += " (" + report.value("category", std::string("error")) + ", exit " +
                std::to_string(report.value("exitCode", 0)) + ")";
            message = report.value("what", std::string("Unknown error"));
            AppendField(message, report, "path", "Path: ");
            AppendField(message, report, "cause", "Cause: ");
            AppendField(message, report, "fix", "Fix: ");
            AppendField(message, report, "log", "Log: ");
        }
    }

    tinyfd_messageBox(WithoutQuotes(title).c_str(), WithoutQuotes(message).c_str(), "ok", "error", 1);
    return 0;
}
