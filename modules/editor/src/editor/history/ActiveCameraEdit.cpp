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
#include <renderer/components/RenderTexture.h>

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace mir
{
    namespace
    {
        std::optional<uint32_t> ActiveField(const ComponentOps &ops)
        {
            if (ops.mType != TypeIdOf<Camera>())
                return std::nullopt;
            const FieldDesc *field = ops.FindField("active");
            if (field == nullptr)
                return std::nullopt;
            return static_cast<uint32_t>(field - ops.mFields.data());
        }

        bool TurnsOn(const FieldValue &value, uint32_t field)
        {
            bool on = false;
            std::memcpy(&on, value.mBytes, sizeof(bool));
            return value.mField == field && on;
        }
    }

    bool CameraActiveLocked(const World &world, Entity entity, const ComponentOps &ops, uint32_t field)
    {
        const std::optional<uint32_t> active = ActiveField(ops);
        return active && *active == field && world.Has<RenderTexture>(entity);
    }

    std::unique_ptr<ICommand> MakeFieldEditCommand(World &world, Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name, IAuthoredEntities *authored)
    {
        const std::optional<uint32_t> activeField = ActiveField(ops);
        if (activeField && world.Has<RenderTexture>(entity))
        {
            std::erase_if(values, [&](const FieldValue &value) { return TurnsOn(value, *activeField); });
            if (values.empty())
                return nullptr;
        }
        const bool activates = activeField &&
            std::any_of(values.begin(), values.end(),
                [&](const FieldValue &value) { return TurnsOn(value, *activeField); });

        std::unique_ptr<ICommand> edit = MakeSetFieldsEditCommand(entity, ops, std::move(values), name, authored);
        if (!activates || edit == nullptr)
            return edit;

        std::vector<std::unique_ptr<ICommand>> commands;
        commands.push_back(std::move(edit));
        world.GetOrCreateQuery<const Camera>().ForEach([&](Entity other, const Camera &camera)
            {
                if (other == entity || !camera.mActive || world.Has<RenderTexture>(other))
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
