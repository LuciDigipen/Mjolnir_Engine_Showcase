/**
 * @file EntityCommands.h
 * @author Dong Ta
 * @brief Create, delete, duplicate and reparent as undoable commands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <editor/history/Command.h>

#include <core/ecs/Entity.h>

#include <memory>
#include <vector>

namespace mir
{
    struct EntityAction;
    struct EntityPresetDesc;

    //null preset is a plain entity
    std::unique_ptr<ICommand> MakeCreateCommand(const EntityPresetDesc *preset);
    std::unique_ptr<ICommand> MakeDeleteCommand(Entity target);
    std::unique_ptr<ICommand> MakeDuplicateCommand(Entity source);
    std::unique_ptr<ICommand> MakeReParentCommand(std::vector<Entity> targets, Entity parent, Entity before);

    //null for EntityActionKind::None
    std::unique_ptr<ICommand> MakeEntityCommand(const EntityAction &action);
}
