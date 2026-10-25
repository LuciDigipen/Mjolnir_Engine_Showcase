/**
 * @file PrefabAuthoringAdapter.cpp
 * @author Jackey Cen
 * @brief App's adapter between the editor's authored commands and the prefab module's authored state.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "PrefabAuthoringAdapter.h"

#include <core/log/Log.h>

#include <core/ecs/EntityPresetRegistry.h>

#include <utility>

namespace mir
{
    namespace
    {
        AuthoredKey ToEditorKey(const PrefabAuthoredKey &key)
        {
            AuthoredKey out;
            if (key.mPrefab)
                out.mObject.mDocument = DocumentId{ key.mPrefab->mHigh, key.mPrefab->mLow };
            else
                out.mObject.mDocument = kSceneDocument;
            out.mObject.mAddress = key.mAddress;
            out.mComponent = key.mComponent;
            out.mField = key.mField;
            return out;
        }

        PrefabAuthoredKey ToPrefabKey(const AuthoredKey &key)
        {
            PrefabAuthoredKey out;
            if (key.mObject.mDocument != kSceneDocument)
            {
                out.mPrefab = PrefabId{ key.mObject.mDocument.mHigh, key.mObject.mDocument.mLow };
            }
            out.mAddress = key.mObject.mAddress;
            out.mComponent = key.mComponent;
            out.mField = key.mField;
            return out;
        }

        AuthoredFieldState ToEditorState(PrefabFieldState state)
        {
            return state == PrefabFieldState::Overridden ? AuthoredFieldState::Overridden
                                                         : AuthoredFieldState::Inherited;
        }

        AuthoredComponentState ToEditorState(PrefabComponentState state)
        {
            switch (state)
            {
            case PrefabComponentState::Inherited:
                return AuthoredComponentState::Inherited;
            case PrefabComponentState::Added:
                return AuthoredComponentState::Added;
            case PrefabComponentState::Removed:
                return AuthoredComponentState::Removed;
            case PrefabComponentState::Absent:
                break;
            }
            return AuthoredComponentState::Absent;
        }

        AuthoredAction ToEditorAction(const PrefabAuthoredAction &action, std::string name)
        {
            AuthoredAction out;
            out.mAllowed = action.mRefusal.empty();
            out.mReason = action.mRefusal;
            out.mName = std::move(name);
            out.mEntries.reserve(action.mEntries.size());
            for (const PrefabAuthoredEntry &entry : action.mEntries)
                out.mEntries.push_back(AuthoredEntry{ ToEditorKey(entry.mKey), entry.mValue });
            return out;
        }

        AuthoredAction RefusedAction(std::string name, std::string reason)
        {
            AuthoredAction out;
            out.mAllowed = false;
            out.mReason = std::move(reason);
            out.mName = std::move(name);
            return out;
        }
    }

    PrefabAuthoringAdapter::PrefabAuthoringAdapter(World &world, PrefabCoordinator &coordinator)
        : mCoordinator(coordinator), mAuthoring(world, coordinator)
    {
    }

    std::optional<std::vector<AuthoredEntry>> PrefabAuthoringAdapter::Read(std::span<const AuthoredKey> keys)
    {
        std::vector<PrefabAuthoredKey> converted;
        converted.reserve(keys.size());
        for (const AuthoredKey &key : keys)
            converted.push_back(ToPrefabKey(key));

        const std::optional<std::vector<PrefabAuthoredEntry>> entries = mAuthoring.Read(converted);
        if (!entries)
            return std::nullopt;

        std::vector<AuthoredEntry> out;
        out.reserve(entries->size());
        for (const PrefabAuthoredEntry &entry : *entries)
            out.push_back(AuthoredEntry{ ToEditorKey(entry.mKey), entry.mValue });
        return out;
    }

    AuthoringWrite PrefabAuthoringAdapter::Write(std::span<const AuthoredEntry> entries)
    {
        std::vector<PrefabAuthoredEntry> converted;
        converted.reserve(entries.size());
        for (const AuthoredEntry &entry : entries)
            converted.push_back(PrefabAuthoredEntry{ ToPrefabKey(entry.mKey), entry.mValue });

        const PrefabAuthoredWrite write = mAuthoring.Write(converted);
        if (!write.mApplied)
            return {};

        AuthoringWrite out;
        out.mApplied = true;
        if (write.mSceneMaintained)
            out.mMaintained.push_back(kSceneDocument);
        return out;
    }

    std::optional<AuthoredKey> PrefabAuthoringAdapter::KeyOf(Entity entity, std::string_view component,
        std::string_view field)
    {
        const std::optional<PrefabAuthoredKey> key = mAuthoring.FieldKey(entity, component, field);
        if (!key)
            return std::nullopt;
        return ToEditorKey(*key);
    }

    std::optional<AuthoredKey> PrefabAuthoringAdapter::ComponentKeyOf(Entity entity, std::string_view component)
    {
        const std::optional<PrefabAuthoredKey> key = mAuthoring.ComponentKey(entity, component);
        if (!key)
            return std::nullopt;
        return ToEditorKey(*key);
    }

    AuthoredFieldState PrefabAuthoringAdapter::FieldState(const AuthoredKey &key)
    {
        return ToEditorState(mAuthoring.FieldState(ToPrefabKey(key)));
    }

    AuthoredComponentState PrefabAuthoringAdapter::ComponentState(Entity entity, std::string_view component)
    {
        return ToEditorState(mAuthoring.ComponentState(entity, component));
    }

    std::vector<std::string> PrefabAuthoringAdapter::RemovedComponents(Entity entity)
    {
        return mAuthoring.RemovedComponents(entity);
    }

    AuthoredInstanceRole PrefabAuthoringAdapter::InstanceRole(Entity entity)
    {
        switch (mAuthoring.InstanceRole(entity))
        {
        case PrefabInstanceRole::Root:
            return AuthoredInstanceRole::Root;
        case PrefabInstanceRole::Member:
            return AuthoredInstanceRole::Member;
        case PrefabInstanceRole::Ordinary:
            break;
        }
        return AuthoredInstanceRole::Ordinary;
    }

    std::vector<Entity> PrefabAuthoringAdapter::PrefabLinkedEntities()
    {
        return mAuthoring.PrefabLinkedEntities();
    }

    std::string PrefabAuthoringAdapter::EncodeField(FieldKind kind, const void *bytes)
    {
        return mAuthoring.EncodeField(kind, bytes);
    }

    AuthoredAction PrefabAuthoringAdapter::Revert(const AuthoredKey &key)
    {
        return ToEditorAction(mAuthoring.Revert(ToPrefabKey(key)), "Revert Override");
    }

    AuthoredAction PrefabAuthoringAdapter::Apply(const AuthoredKey &key)
    {
        return ToEditorAction(mAuthoring.Apply(ToPrefabKey(key)), "Apply to Prefab");
    }

    AuthoredAction PrefabAuthoringAdapter::CreateNode(Entity parent, std::size_t siblingIndex,
        const EntityPresetDesc *preset)
    {
        const std::optional<PrefabNodeId> parentNode = mAuthoring.NodeIdOf(parent);
        if (!parentNode)
            return RefusedAction("Create Node", "the parent is not part of the active prefab document");

        std::string name = "Create Node";
        if (preset != nullptr)
            name = "Create " + std::string(preset->mName.substr(preset->mName.find('/') + 1));
        return ToEditorAction(mAuthoring.CreateNode(*parentNode, siblingIndex, preset), std::move(name));
    }

    AuthoredAction PrefabAuthoringAdapter::DeleteNodes(std::span<const Entity> nodes)
    {
        if (nodes.empty())
            return RefusedAction("Delete Node", "no node is selected");

        std::vector<PrefabNodeId> ids;
        ids.reserve(nodes.size());
        for (const Entity entity : nodes)
        {
            const std::optional<PrefabNodeId> id = mAuthoring.NodeIdOf(entity);
            if (!id)
                return RefusedAction("Delete Node", "a selected entity is not part of the active prefab document");
            ids.push_back(*id);
        }
        return ToEditorAction(mAuthoring.DeleteNodes(ids), "Delete Node");
    }

    AuthoredAction PrefabAuthoringAdapter::ReparentNode(Entity node, Entity parent, std::size_t siblingIndex)
    {
        const std::optional<PrefabNodeId> nodeId = mAuthoring.NodeIdOf(node);
        const std::optional<PrefabNodeId> parentId = mAuthoring.NodeIdOf(parent);
        if (!nodeId || !parentId)
            return RefusedAction("Reparent Node", "the node and parent must be part of the active prefab document");
        return ToEditorAction(mAuthoring.ReparentNode(*nodeId, *parentId, siblingIndex), "Reparent Node");
    }

    bool PrefabAuthoringAdapter::WritesComponentDirectly(Entity entity, std::string_view component)
    {
        return mAuthoring.WritesComponentDirectly(entity, component);
    }

    bool PrefabAuthoringAdapter::SaveScenePrefabs(const LoadedScene &scene, const PrefabUnsavedPredicate &unsaved,
        const std::function<void(PrefabId)> &saved)
    {
        const PrefabSavePlan plan = PlanSaveScene(mCoordinator.Registry(), scene, unsaved);
        const PrefabSaveResult result = ExecuteSavePlan(mCoordinator.Registry(), plan);
        for (PrefabId id : result.mWritten)
            saved(id);
        for (const PrefabDiagnostic &diagnostic : result.mDiagnostics)
            MIR_LOG_WARN("Prefab save: {}", diagnostic.mMessage);
        return result.mSucceeded;
    }
}
