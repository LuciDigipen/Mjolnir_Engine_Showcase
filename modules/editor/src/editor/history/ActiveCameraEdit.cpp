/**
 * @file ActiveCameraEdit.cpp
 * @author Quy Dong Ta
 * @brief Inspector field edits, where turning a Camera active is one undo step with clearing the others.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/ActiveCameraEdit.h>

#include <editor/history/AuthoredEdit.h>
#include <editor/history/CommandGroup.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>
#include <renderer/components/Camera.h>

#include <cstring>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        
        std::optional<uint32_t> ActivatedField(const ComponentOps &ops, const std::vector<FieldValue> &values)
        {
            if (ops.mType != TypeIdOf<Camera>())
                return std::nullopt;
            const FieldDesc *field = ops.FindField("active");
            if (field == nullptr)
                return std::nullopt;

            const auto index = static_cast<uint32_t>(field - ops.mFields.data());
            for (const FieldValue &value : values)
            {
                bool on = false;
                std::memcpy(&on, value.mBytes, sizeof(bool));
                if (value.mField == index && on)
                    return index;
            }
            return std::nullopt;
        }
    }

    std::unique_ptr<ICommand> MakeFieldEditCommand(World &world, Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name, IAuthoredEntities *authored)
    {
        const std::optional<uint32_t> activeField = ActivatedField(ops, values);
        std::unique_ptr<ICommand> edit = MakeSetFieldsEditCommand(entity, ops, std::move(values), name, authored);
        if (!activeField || edit == nullptr)
            return edit;

        std::vector<std::unique_ptr<ICommand>> commands;
        commands.push_back(std::move(edit));
        world.GetOrCreateQuery<const Camera>().ForEach([&](Entity other, const Camera &camera)
            {
                if (other == entity || !camera.mActive)
                    return;
                FieldValue off;
                off.mField = *activeField;
                commands.push_back(MakeSetFieldsEditCommand(other, ops, { off }, "", authored));
            });

        if (commands.size() == 1)
            return std::move(commands.front());
        return MakeCommandGroup(name.empty() ? "Set Active Camera" : std::move(name), std::move(commands));
    }
}
