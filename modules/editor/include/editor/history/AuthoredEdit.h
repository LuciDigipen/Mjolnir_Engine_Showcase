/**
 * @file AuthoredEdit.h
 * @author Jackey Cen
 * @brief Field, component and override edits routed between World commands and authored commands.
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
#include <span>
#include <string>
#include <vector>

namespace mir
{
    struct ComponentOps;

    // Uses the authored command when the entity is a Prefab instance member, else the World command.
    std::unique_ptr<ICommand> MakeSetFieldsEditCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name, IAuthoredEntities *authored);

    std::unique_ptr<ICommand> MakeAddComponentEditCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, IAuthoredEntities *authored);

    std::unique_ptr<ICommand> MakeRemoveComponentEditCommand(Entity entity, const ComponentOps &ops,
        IAuthoredEntities *authored);

    // Null and logged when the view refuses the action.
    std::unique_ptr<ICommand> MakeRevertAuthoredCommand(IAuthoredEntities &authored, const AuthoredKey &key);
    std::unique_ptr<ICommand> MakeApplyAuthoredCommand(IAuthoredEntities &authored, const AuthoredKey &key,
        std::size_t target = 0);

    // Several keys as one command, as a field and the fields a pick writes with it; null and logged when
    // the view refuses any of them, so none is reverted or applied without the rest.
    std::unique_ptr<ICommand> MakeRevertAuthoredCommand(IAuthoredEntities &authored,
        std::span<const AuthoredKey> keys);
    std::unique_ptr<ICommand> MakeApplyAuthoredCommand(IAuthoredEntities &authored,
        std::span<const AuthoredKey> keys, std::size_t target = 0);

    // The Hierarchy's Create Prefab, as one authored command over the new document.
    std::unique_ptr<ICommand> MakeCreatePrefabCommand(IAuthoredEntities &authored, Entity entity);
}
