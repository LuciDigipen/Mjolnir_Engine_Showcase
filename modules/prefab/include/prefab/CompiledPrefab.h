/**
 * @file CompiledPrefab.h
 * @author Jackey Cen
 * @brief Immutable component defaults and EntityRef relocations for one prefab revision.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "prefab/PrefabDocument.h"
#include "prefab/PrefabFormat.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace mir
{
    struct ComponentOps;
    struct PrefabCompileResult;

    struct CompiledPrefabComponent
    {
        const ComponentOps *mOps = nullptr;
        std::vector<std::byte> mBytes;
    };

    struct CompiledPrefabNode
    {
        PrefabNodeAddress mAddress;
        std::optional<std::size_t> mParent;
        std::vector<CompiledPrefabComponent> mComponents;
    };

    struct PrefabEntityRefRelocation
    {
        std::size_t mNode = 0;
        std::size_t mComponent = 0;
        std::size_t mField = 0;
        std::size_t mTargetNode = 0;
    };

    class CompiledPrefab
    {
    public:
        const PrefabId &Id() const { return mId; }

        // Assigned by CompiledPrefabCache; zero for a direct CompilePrefab result.
        uint64_t Revision() const { return mRevision; }

        std::span<const CompiledPrefabNode> Nodes() const { return mNodes; }
        std::span<const PrefabEntityRefRelocation> Relocations() const { return mRelocations; }

    private:
        friend class CompiledPrefabCache;
        friend PrefabCompileResult CompilePrefab(const PrefabDocument &doc);

        CompiledPrefab(PrefabId id, uint64_t revision, std::vector<CompiledPrefabNode> nodes,
            std::vector<PrefabEntityRefRelocation> relocations);

        PrefabId mId{};
        uint64_t mRevision = 0;
        std::vector<CompiledPrefabNode> mNodes;
        std::vector<PrefabEntityRefRelocation> mRelocations;
    };

    struct PrefabCompileResult
    {
        std::shared_ptr<const CompiledPrefab> mPrefab;
        std::vector<PrefabDiagnostic> mDiagnostics;
    };

    PrefabCompileResult CompilePrefab(const PrefabDocument &doc);

    class CompiledPrefabCache
    {
    public:
        // A successful compile replaces the entry and increments its revision;
        // a failed compile leaves the last valid template and revision untouched.
        std::vector<PrefabDiagnostic> Compile(const PrefabDocument &doc);

        // Null until a compile of this id succeeds.
        std::shared_ptr<const CompiledPrefab> Find(PrefabId id) const;
        void Remove(PrefabId id);

    private:
        std::unordered_map<PrefabId, std::shared_ptr<const CompiledPrefab>> mEntries;
    };
}
