/**
 * @file SceneInstanceCodec.cpp
 * @author Jackey Cen
 * @brief Scene prefab instance <-> Json converter and record patch helpers.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "SceneInstanceCodec.h"

#include "EntityCodec.h"
#include "core/log/Log.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mir
{
    namespace
    {
        using nlohmann::json;

        std::optional<uint64_t> ParseNodeIdText(std::string_view text)
        {
            if (text.empty() || (text.size() > 1 && text.front() == '0'))
                return std::nullopt;

            uint64_t value = 0;
            for (char c : text)
            {
                if (c < '0' || c > '9')
                    return std::nullopt;
                const uint64_t digit = static_cast<uint64_t>(c - '0');
                if (value > (std::numeric_limits<uint64_t>::max() - digit) / 10)
                    return std::nullopt;
                value = value * 10 + digit;
            }
            if (value == 0)
                return std::nullopt;
            return value;
        }

        std::optional<SceneNodeAddress> ReadAddress(const json &value)
        {
            if (!value.is_array() || value.empty())
                return std::nullopt;

            SceneNodeAddress address;
            for (const json &segment : value)
            {
                if (!segment.is_string())
                    return std::nullopt;

                const std::optional<uint64_t> id = ParseNodeIdText(segment.get_ref<const std::string &>());
                if (!id)
                    return std::nullopt;
                address.push_back(*id);
            }
            return address;
        }

        json AddressToJson(const SceneNodeAddress &address)
        {
            json out = json::array();
            for (uint64_t id : address)
                out.push_back(std::to_string(id));
            return out;
        }

        std::optional<uint32_t> ReadSiblingIndex(const json &value)
        {
            if (value.is_number_unsigned())
            {
                const uint64_t raw = value.get<uint64_t>();
                if (raw > std::numeric_limits<uint32_t>::max())
                    return std::nullopt;
                return static_cast<uint32_t>(raw);
            }
            if (value.is_number_integer())
            {
                const int64_t raw = value.get<int64_t>();
                if (raw < 0 || raw > std::numeric_limits<uint32_t>::max())
                    return std::nullopt;
                return static_cast<uint32_t>(raw);
            }
            return std::nullopt;
        }

        bool IsInstancePatchEmpty(const SceneInstancePatch &patch)
        {
            return patch.mFields.empty() && patch.mAddedComponents.empty() && patch.mRemovedComponents.empty();
        }
    }

    json InstanceToJson(const ScenePrefabInstance &instance)
    {
        json file;
        file["root"] = instance.mRoot;
        file["prefab"] = instance.mPrefab;
        file["parent"] = instance.mParent;
        file["siblingIndex"] = instance.mSiblingIndex;
        file["placement"] = instance.mPlacement;

        json bindings = json::array();
        for (const auto &[address, id] : instance.mBindings)
            bindings.push_back(json{ { "address", AddressToJson(address) }, { "id", id } });
        file["bindings"] = std::move(bindings);

        json patches = json::array();
        for (const SceneInstancePatch &patch : instance.mPatches)
        {
            json out;
            out["target"] = AddressToJson(patch.mTarget);

            if (!patch.mFields.empty())
            {
                json fields = json::array();
                for (const SceneFieldPatch &field : patch.mFields)
                {
                    fields.push_back(json{
                        { "component", field.mComponent },
                        { "field", field.mField },
                        { "value", field.mValue },
                    });
                }
                out["fields"] = std::move(fields);
            }

            if (!patch.mAddedComponents.empty())
            {
                json added = json::array();
                for (const SceneComponentRecord &component : patch.mAddedComponents)
                    added.push_back(json{ { "type", component.mType }, { "fields", component.mFields } });
                out["addedComponents"] = std::move(added);
            }

            if (!patch.mRemovedComponents.empty())
                out["removedComponents"] = patch.mRemovedComponents;

            patches.push_back(std::move(out));
        }
        file["patches"] = std::move(patches);

        return file;
    }

    std::optional<ScenePrefabInstance> InstanceFromJson(const json &file, std::string_view context)
    {
        const auto fail = [context](const std::string &message) -> std::optional<ScenePrefabInstance>
            {
                MIR_LOG_ERROR("LoadScene: instance '{}' {}", context, message);
                return std::nullopt;
            };

        if (!file.is_object())
            return fail("is not a JSON object");

        const auto rootIt = file.find("root");
        if (rootIt == file.end())
            return fail("is missing \"root\"");
        const std::optional<StableId> root = ReadStableIdJson(*rootIt);
        if (!root || *root == kNullStableId)
            return fail("has an invalid \"root\"");

        const auto prefabIt = file.find("prefab");
        if (prefabIt == file.end() || !prefabIt->is_string() || prefabIt->get_ref<const std::string &>().empty())
            return fail("has an invalid \"prefab\"");

        ScenePrefabInstance instance;
        instance.mRoot = *root;
        instance.mPrefab = prefabIt->get<std::string>();

        const auto parentIt = file.find("parent");
        if (parentIt != file.end())
        {
            const std::optional<StableId> parent = ReadStableIdJson(*parentIt);
            if (!parent)
                return fail("has an invalid \"parent\"");
            instance.mParent = *parent;
        }

        const auto siblingIt = file.find("siblingIndex");
        if (siblingIt != file.end())
        {
            const std::optional<uint32_t> siblingIndex = ReadSiblingIndex(*siblingIt);
            if (!siblingIndex)
                return fail("has an invalid \"siblingIndex\"");
            instance.mSiblingIndex = *siblingIndex;
        }

        const auto placementIt = file.find("placement");
        if (placementIt != file.end())
        {
            if (!placementIt->is_object())
                return fail("has an invalid \"placement\"");
            instance.mPlacement = *placementIt;
        }

        const auto bindingsIt = file.find("bindings");
        if (bindingsIt != file.end())
        {
            if (!bindingsIt->is_array())
                return fail("has an invalid \"bindings\"");

            for (const json &entry : *bindingsIt)
            {
                if (!entry.is_object())
                    return fail("has a binding that is not an object");

                const auto addressIt = entry.find("address");
                const auto idIt = entry.find("id");
                if (addressIt == entry.end() || idIt == entry.end())
                    return fail("has a binding without an address or id");

                const std::optional<SceneNodeAddress> address = ReadAddress(*addressIt);
                const std::optional<StableId> id = ReadStableIdJson(*idIt);
                if (!address || !id || *id == kNullStableId)
                    return fail("has an invalid binding address or id");
                instance.mBindings[*address] = *id;
            }
        }

        const auto patchesIt = file.find("patches");
        if (patchesIt != file.end())
        {
            if (!patchesIt->is_array())
                return fail("has an invalid \"patches\"");

            for (const json &patchJson : *patchesIt)
            {
                if (!patchJson.is_object())
                    return fail("has a patch that is not an object");

                const auto targetIt = patchJson.find("target");
                if (targetIt == patchJson.end())
                    return fail("has a patch without a target");
                const std::optional<SceneNodeAddress> target = ReadAddress(*targetIt);
                if (!target)
                    return fail("has a patch with an invalid target");

                SceneInstancePatch patch;
                patch.mTarget = *target;

                const auto fieldsIt = patchJson.find("fields");
                if (fieldsIt != patchJson.end())
                {
                    if (!fieldsIt->is_array())
                        return fail("has a patch with invalid fields");

                    for (const json &fieldJson : *fieldsIt)
                    {
                        if (!fieldJson.is_object())
                            return fail("has a field patch that is not an object");

                        const auto componentIt = fieldJson.find("component");
                        const auto fieldIt = fieldJson.find("field");
                        const auto valueIt = fieldJson.find("value");
                        if (componentIt == fieldJson.end() || fieldIt == fieldJson.end() ||
                            valueIt == fieldJson.end() || !componentIt->is_string() || !fieldIt->is_string())
                        {
                            return fail("has a field patch without a component, field or value");
                        }

                        patch.mFields.push_back(SceneFieldPatch{
                            componentIt->get<std::string>(), fieldIt->get<std::string>(), *valueIt });
                    }
                }

                const auto addedIt = patchJson.find("addedComponents");
                if (addedIt != patchJson.end())
                {
                    if (!addedIt->is_array())
                        return fail("has a patch with invalid addedComponents");

                    for (const json &componentJson : *addedIt)
                    {
                        if (!componentJson.is_object())
                            return fail("has an added component that is not an object");

                        const auto typeIt = componentJson.find("type");
                        if (typeIt == componentJson.end() || !typeIt->is_string() ||
                            typeIt->get_ref<const std::string &>().empty())
                        {
                            return fail("has an added component without a type");
                        }

                        SceneComponentRecord component;
                        component.mType = typeIt->get<std::string>();

                        const auto componentFieldsIt = componentJson.find("fields");
                        if (componentFieldsIt != componentJson.end())
                        {
                            if (!componentFieldsIt->is_object())
                                return fail("has an added component with invalid fields");
                            component.mFields = *componentFieldsIt;
                        }
                        patch.mAddedComponents.push_back(std::move(component));
                    }
                }

                const auto removedIt = patchJson.find("removedComponents");
                if (removedIt != patchJson.end())
                {
                    if (!removedIt->is_array())
                        return fail("has a patch with invalid removedComponents");

                    for (const json &removed : *removedIt)
                    {
                        if (!removed.is_string())
                            return fail("has a removed component that is not a string");
                        patch.mRemovedComponents.push_back(removed.get<std::string>());
                    }
                }

                instance.mPatches.push_back(std::move(patch));
            }
        }

        return instance;
    }

    SceneInstancePatch *FindInstancePatch(ScenePrefabInstance &instance, const SceneNodeAddress &target)
    {
        for (SceneInstancePatch &patch : instance.mPatches)
        {
            if (patch.mTarget == target)
                return &patch;
        }
        return nullptr;
    }

    const SceneInstancePatch *FindInstancePatch(const ScenePrefabInstance &instance, const SceneNodeAddress &target)
    {
        for (const SceneInstancePatch &patch : instance.mPatches)
        {
            if (patch.mTarget == target)
                return &patch;
        }
        return nullptr;
    }

    SceneInstancePatch &GetOrAddInstancePatch(ScenePrefabInstance &instance, const SceneNodeAddress &target)
    {
        if (SceneInstancePatch *patch = FindInstancePatch(instance, target))
            return *patch;

        SceneInstancePatch patch;
        patch.mTarget = target;
        instance.mPatches.push_back(std::move(patch));
        return instance.mPatches.back();
    }

    SceneFieldPatch *FindInstanceFieldPatch(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field)
    {
        SceneInstancePatch *patch = FindInstancePatch(instance, target);
        if (patch == nullptr)
            return nullptr;

        for (SceneFieldPatch &fieldPatch : patch->mFields)
        {
            if (fieldPatch.mComponent == component && fieldPatch.mField == field)
                return &fieldPatch;
        }
        return nullptr;
    }

    const SceneFieldPatch *FindInstanceFieldPatch(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field)
    {
        const SceneInstancePatch *patch = FindInstancePatch(instance, target);
        if (patch == nullptr)
            return nullptr;

        for (const SceneFieldPatch &fieldPatch : patch->mFields)
        {
            if (fieldPatch.mComponent == component && fieldPatch.mField == field)
                return &fieldPatch;
        }
        return nullptr;
    }

    bool EraseInstanceFieldPatch(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component, std::string_view field)
    {
        for (auto patch = instance.mPatches.begin(); patch != instance.mPatches.end(); ++patch)
        {
            if (patch->mTarget != target)
                continue;

            const std::size_t before = patch->mFields.size();
            std::erase_if(patch->mFields, [&](const SceneFieldPatch &fieldPatch)
                {
                    return fieldPatch.mComponent == component && fieldPatch.mField == field;
                });
            if (patch->mFields.size() == before)
                return false;

            if (IsInstancePatchEmpty(*patch))
                instance.mPatches.erase(patch);
            return true;
        }
        return false;
    }

    SceneComponentRecord *FindAddedComponent(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component)
    {
        SceneInstancePatch *patch = FindInstancePatch(instance, target);
        if (patch == nullptr)
            return nullptr;

        for (SceneComponentRecord &record : patch->mAddedComponents)
        {
            if (record.mType == component)
                return &record;
        }
        return nullptr;
    }

    const SceneComponentRecord *FindAddedComponent(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component)
    {
        const SceneInstancePatch *patch = FindInstancePatch(instance, target);
        if (patch == nullptr)
            return nullptr;

        for (const SceneComponentRecord &record : patch->mAddedComponents)
        {
            if (record.mType == component)
                return &record;
        }
        return nullptr;
    }

    bool EraseAddedComponent(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component)
    {
        for (auto patch = instance.mPatches.begin(); patch != instance.mPatches.end(); ++patch)
        {
            if (patch->mTarget != target)
                continue;

            const std::size_t before = patch->mAddedComponents.size();
            std::erase_if(patch->mAddedComponents, [&](const SceneComponentRecord &record)
                {
                    return record.mType == component;
                });
            if (patch->mAddedComponents.size() == before)
                return false;

            if (IsInstancePatchEmpty(*patch))
                instance.mPatches.erase(patch);
            return true;
        }
        return false;
    }

    bool HasRemovedComponent(const ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component)
    {
        const SceneInstancePatch *patch = FindInstancePatch(instance, target);
        if (patch == nullptr)
            return false;

        for (const std::string &removed : patch->mRemovedComponents)
        {
            if (removed == component)
                return true;
        }
        return false;
    }

    bool EraseRemovedComponent(ScenePrefabInstance &instance, const SceneNodeAddress &target,
        std::string_view component)
    {
        for (auto patch = instance.mPatches.begin(); patch != instance.mPatches.end(); ++patch)
        {
            if (patch->mTarget != target)
                continue;

            const std::size_t before = patch->mRemovedComponents.size();
            std::erase_if(patch->mRemovedComponents, [&](const std::string &removed)
                {
                    return removed == component;
                });
            if (patch->mRemovedComponents.size() == before)
                return false;

            if (IsInstancePatchEmpty(*patch))
                instance.mPatches.erase(patch);
            return true;
        }
        return false;
    }
}
