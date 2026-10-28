/**
 * @file CommandGroup.h
 * @author Quy Dong Ta
 * @brief Several commands as one undo step.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>

#include <memory>
#include <string>
#include <vector>

namespace mir
{
    // Does the commands in order and undoes them in reverse
    std::unique_ptr<ICommand> MakeCommandGroup(std::string name, std::vector<std::unique_ptr<ICommand>> commands);
}
