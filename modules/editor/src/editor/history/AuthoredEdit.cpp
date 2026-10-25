/**
 * @file AuthoredEdit.cpp
 * @author Jackey Cen
 * @brief Field, component and override edits routed between World commands and authored commands.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/AuthoredEdit.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/log/Log.h>

#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace mir
{
    namespace
    {
        std::string FieldEditName(const ComponentOps &ops, const std::vector<FieldValue> &values,
            const std::string &name)
        {
            if (!name.empty())
                return name;
            if (values.size() == 1)
            {
                return std::format("Edit {}.{}", ops.mType.name,
                    ops.mFields[values.front().mField].mName);
            }
            return std::format("Edit {}", ops.mType.name);
        }

        std::optional<std::vector<AuthoredEntry>> FieldEntries(Entity entity, const ComponentOps &ops,
            const std::vector<FieldValue> &values, IAuthoredEntities &authored)
        {
            std::vector<AuthoredEntry> entries;
            entries.reserve(values.size());
            for (const FieldValue &value : values)
            {
                const FieldDesc &field = ops.mFields[value.mField];
                const std::optional<AuthoredKey> key = authored.KeyOf(entity, ops.mType.name, field.mName);
                if (!key)
                    return std::nullopt;
                entries.push_back(AuthoredEntry{ *key, authored.EncodeField(field.mKind, value.mBytes) });
            }
            return entries;
        }

        std::string ComponentName(const ComponentOps &ops, const std::vector<FieldValue> &values)
        {
            return std::format("{} {}", values.empty() ? "Add" : "Paste", ops.mType.name);
        }

        std::unique_ptr<ICommand> RefusedCommand(const char *name, const AuthoredAction &action)
        {
            MIR_LOG_WARN("Editor: {} refused: {}", name, action.mReason);
            return nullptr;
        }
    }

    std::unique_ptr<ICommand> MakeSetFieldsEditCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, std::string name, IAuthoredEntities *authored)
    {
        if (authored != nullptr && !values.empty())
        {
            const std::string commandName = FieldEditName(ops, values, name);
            if (std::optional<std::vector<AuthoredEntry>> entries = FieldEntries(entity, ops, values, *authored))
                return MakeAuthoredCommand(commandName, std::move(*entries));
        }

        return MakeSetFieldsCommand(entity, ops, std::move(values), std::move(name));
    }

    std::unique_ptr<ICommand> MakeAddComponentEditCommand(Entity entity, const ComponentOps &ops,
        std::vector<FieldValue> values, IAuthoredEntities *authored)
    {
        if (authored != nullptr)
        {
            const std::optional<AuthoredKey> key = authored->ComponentKeyOf(entity, ops.mType.name);
            if (key)
            {
                nlohmann::json fields = nlohmann::json::object();
                for (const FieldValue &value : values)
                {
                    const FieldDesc &field = ops.mFields[value.mField];
                    fields[std::string(field.mName)] =
                        nlohmann::json::parse(authored->EncodeField(field.mKind, value.mBytes));
                }

                return MakeAuthoredCommand(ComponentName(ops, values),
                    { AuthoredEntry{ *key, fields.dump() } });
            }
        }

        return MakeAddComponentCommand(entity, ops, std::move(values));
    }

    std::unique_ptr<ICommand> MakeRemoveComponentEditCommand(Entity entity, const ComponentOps &ops,
        IAuthoredEntities *authored)
    {
        if (authored != nullptr)
        {
            const std::optional<AuthoredKey> key = authored->ComponentKeyOf(entity, ops.mType.name);
            if (key)
            {
                const AuthoredComponentState state = authored->ComponentState(entity, ops.mType.name);
                std::optional<std::string> value;
                if (state == AuthoredComponentState::Inherited &&
                    !authored->WritesComponentDirectly(entity, ops.mType.name))
                {
                    value = std::string("null");
                }
                return MakeAuthoredCommand(std::format("Remove {}", ops.mType.name),
                    { AuthoredEntry{ *key, std::move(value) } });
            }
        }

        return MakeRemoveComponentCommand(entity, ops);
    }

    std::unique_ptr<ICommand> MakeRevertAuthoredCommand(IAuthoredEntities &authored, const AuthoredKey &key)
    {
        AuthoredAction action = authored.Revert(key);
        if (!action.mAllowed)
            return RefusedCommand("Revert", action);
        return MakeAuthoredCommand(action.mName.empty() ? "Revert Override" : action.mName,
            std::move(action.mEntries));
    }

    std::unique_ptr<ICommand> MakeApplyAuthoredCommand(IAuthoredEntities &authored, const AuthoredKey &key)
    {
        AuthoredAction action = authored.Apply(key);
        if (!action.mAllowed)
            return RefusedCommand("Apply to Prefab", action);
        return MakeAuthoredCommand(action.mName.empty() ? "Apply to Prefab" : action.mName,
            std::move(action.mEntries));
    }
}
