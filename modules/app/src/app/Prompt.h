/**
 * @file Prompt.h
 * @author sumin.park
 * @brief For OS based prompt box.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <string_view>

namespace mir
{
    // No is always the safe answer: --non-interactive takes it without showing anything
    bool AskYesNo(std::string_view title, std::string_view message, bool nonInteractive);
}
