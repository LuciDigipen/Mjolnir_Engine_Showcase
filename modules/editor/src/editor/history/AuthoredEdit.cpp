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
#include <iterator>
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

        // every key's entries in one command, named by the first action, or none when any is refused
        template <typename ActionOf>
        std::unique_ptr<ICommand> CombinedCommand(const char *verb, const char *fallbackName,
            std::span<const AuthoredKey> keys, ActionOf actionOf)
        {
            std::string name;
            std::vector<AuthoredEntry> entries;
            for (const AuthoredKey &key : keys)
            {
                AuthoredAction action = actionOf(key);
                if (!action.mAllowed)
                    return RefusedCommand(verb, action);
                if (name.empty())
                    name = std::move(action.mName);
                entries.insert(entries.end(), std::make_move_iterator(action.mEntries.begin()),
                    std::make_move_iterator(action.mEntries.end()));
            }
            return MakeAuthoredCommand(name.empty() ? fallbackName : name, std::move(entries));
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
        return MakeRevertAuthoredCommand(authored, std::span<const AuthoredKey>(&key, 1));
    }

    std::unique_ptr<ICommand> MakeApplyAuthoredCommand(IAuthoredEntities &authored, const AuthoredKey &key,
        std::size_t target)
    {
        return MakeApplyAuthoredCommand(authored, std::span<const AuthoredKey>(&key, 1), target);
    }

    std::unique_ptr<ICommand> MakeRevertAuthoredCommand(IAuthoredEntities &authored,
        std::span<const AuthoredKey> keys)
    {
        return CombinedCommand("Revert", "Revert Override", keys,
            [&authored](const AuthoredKey &key) { return authored.Revert(key); });
    }

    std::unique_ptr<ICommand> MakeApplyAuthoredCommand(IAuthoredEntities &authored,
        std::span<const AuthoredKey> keys, std::size_t target)
    {
        return CombinedCommand("Apply to Prefab", "Apply to Prefab", keys,
            [&authored, target](const AuthoredKey &key) { return authored.Apply(key, target); });
    }

    std::unique_ptr<ICommand> MakeCreatePrefabCommand(IAuthoredEntities &authored, Entity entity)
    {
        AuthoredAction action = authored.CreatePrefab(entity);
        if (!action.mAllowed)
            return RefusedCommand("Create Prefab", action);
        return MakeAuthoredCommand(action.mName.empty() ? "Create Prefab" : action.mName,
            std::move(action.mEntries));
    }
}
