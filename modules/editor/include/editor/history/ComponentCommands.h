/**
 * @file ComponentCommands.h
 * @author Dong Ta
 * @brief Field edits, add component and remove component as undoable commands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>

#include <core/ecs/Entity.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mir
{
    struct ComponentOps;

    
    struct FieldValue
    {
        uint32_t mField = 0; //index into ComponentOps::mFields
        alignas(16) std::byte mBytes[64]{};
    };

    
    std::unique_ptr<ICommand> MakeSetFieldsCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name = {});

    
    std::unique_ptr<ICommand> MakeAddComponentCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values = {});

    std::unique_ptr<ICommand> MakeRemoveComponentCommand(Entity entity, const ComponentOps &ops);
}