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
#include <core/math/WorldCoordinates.h>

#include <memory>
#include <string>
#include <vector>

namespace mir
{
    struct EntityAction;
    struct EntityPresetDesc;

    //null preset is a plain entity
    std::unique_ptr<ICommand> MakeCreateCommand(const EntityPresetDesc *preset);
    std::unique_ptr<ICommand> MakeDeleteCommand(Entity target);
    std::unique_ptr<ICommand> MakeDuplicateCommand(Entity source);
    std::unique_ptr<ICommand> MakeCopyAsOrdinaryObjectCommand(Entity source);
    std::unique_ptr<ICommand> MakeReParentCommand(std::vector<Entity> targets, Entity parent, Entity before);

    // Places a linked Prefab instance; position is local to parent, or a world position for a root.
    std::unique_ptr<ICommand> MakePlacePrefabCommand(std::string contentPath, Entity parent,
        const WorldVec3 &position);
    std::unique_ptr<ICommand> MakePlaceParticleSystemCommand(std::string contentPath, Entity parent,
        const WorldVec3 &position);

    //null for EntityActionKind::None
    std::unique_ptr<ICommand> MakeEntityCommand(const EntityAction &action);
}
