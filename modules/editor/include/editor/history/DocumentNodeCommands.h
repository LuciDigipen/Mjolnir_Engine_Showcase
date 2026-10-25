/**
 * @file DocumentNodeCommands.h
 * @author Jackey Cen
 * @brief Create, delete and reparent a document's nodes as one authored command each.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/Entity.h>

#include <memory>

namespace mir
{
    class IAuthoredEntities;
    class ICommand;
    class World;
    struct EntityAction;

    // Routes a Hierarchy action against the authored document rooted in @p world.
    // Null for an action the document does not offer.
    std::unique_ptr<ICommand> MakeDocumentEntityCommand(const EntityAction &action, IAuthoredEntities &authored,
        World &world);
}
