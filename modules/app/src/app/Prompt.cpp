/**
 * @file Prompt.cpp
 * @author sumin.park
 * @brief For OS based prompt box.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "Prompt.h"

#include <core/log/Log.h>

#include <tinyfiledialogs.h>

#include <algorithm>
#include <string>

namespace mir
{
    namespace
    {
        // tinyfd refuses a title or message holding a quote and shows a placeholder instead
        std::string WithoutQuotes(std::string_view text)
        {
            std::string out(text);
            std::replace(out.begin(), out.end(), '"', '`');
            std::replace(out.begin(), out.end(), '\'', '`');
            return out;
        }
    }

    bool AskYesNo(std::string_view title, std::string_view message, bool nonInteractive)
    {
        if (nonInteractive)
        {
            MIR_LOG_WARN("{}: {} (--non-interactive answers No)", title, message);
            return false;
        }

        const std::string safeTitle = WithoutQuotes(title);
        const std::string safeMessage = WithoutQuotes(message);
        const bool yes = tinyfd_messageBox(safeTitle.c_str(), safeMessage.c_str(), "yesno", "warning", 0) == 1;
        MIR_LOG_INFO("{}: {} -> {}", title, message, yes ? "Yes" : "No");
        return yes;
    }
}
