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

    // True for a Camera's active field on a camera that renders into a RenderTexture, which is never the active one
    bool CameraActiveLocked(const World &world, Entity entity, const ComponentOps &ops, uint32_t field);

    // MakeSetFieldsEditCommand, except that turning a Camera active also clears every other active
    // xamera in the same step, so undoing the switch brings the previous camera back.
    // A RenderTexture camera is never turned active, nor are the others' flags cleared for it
    std::unique_ptr<ICommand> MakeFieldEditCommand(World &world, Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name, IAuthoredEntities *authored);
}
