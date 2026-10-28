/**
 * @file ActiveCameraEdit.h
 * @author Quy Dong Ta
 * @brief Inspector field edits, where turning a Camera active is one undo step with clearing the others.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/AuthoredView.h>
#include <editor/history/Command.h>
#include <editor/history/ComponentCommands.h>

#include <core/ecs/Entity.h>

#include <memory>
#include <string>
#include <vector>

namespace mir
{
    struct ComponentOps;
    class World;

    // MakeSetFieldsEditCommand, except that turning a Camera active also clears every other active
    // xamera in the same step, so undoing the switch brings the previous camera back.
    std::unique_ptr<ICommand> MakeFieldEditCommand(World &world, Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name, IAuthoredEntities *authored);
}
