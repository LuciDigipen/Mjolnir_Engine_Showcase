/**
 * @file World.h
 * @author Sumin Park
 * @brief Owns entities, components, and the interactions with/of them
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */

#pragma once
#include "Archetype.h"
#include "ComponentAsserts.h"
#include "Entity.h"
#include "EntityPool.h"
#include "HierarchyMutator.h"
#include "QueryTerms.h"
#include "Signature.h"
#include "core/log/Assert.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mir
{
    class World;

    namespace detail
    {
        class ArchetypeMatcher;
        struct QueryIterationGuard;
    }

    struct ComponentHooks
    {
        std::function<void(World &world, Entity entity)> onAdd;
        std::function<void(World &world, Entity entity)> onRemove;
    };

    // entity position of entity's archetype
    struct EntityRecord
    {
        Archetype *archetype = nullptr;
        uint32_t row = 0;
        bool destroying = false; // set while DestroyEntity runs its hooks
    };

    namespace detail
    {
        inline uint32_t NextResourceId()
        {
            static std::atomic<uint32_t> counter{ 0 };
            return counter.fetch_add(1, std::memory_order_relaxed);
        }

        template <typename T>
        uint32_t ResourceIdOf()
        {
            static const uint32_t id = NextResourceId();
            return id;
        }

        template <typename T>
        uint32_t ResourceKeyOf()
        {
            return ResourceIdOf<std::remove_cvref_t<T>>();
        }

        class IResource
        {
        public:
            virtual ~IResource() = default;
        };

        template <typename T>
        class ResourceHolder final : public IResource
        {
        public:
            template <typename... Args>
            explicit ResourceHolder(Args &&...args) : mValue(std::forward<Args>(args)...)
            {}

            T mValue;
        };

    }

    class World
    {
    public:
        World()
        {
            // every entity starts in a table with no columns
            mEmptyArchetype = &GetOrCreateArchetype(Signature{});
        }

        World(const World &) = delete;
        World &operator=(const World &) = delete;
        World(World &&) = delete;
        World &operator=(World &&) = delete;

        Entity CreateEntity()
        {
            const Entity entity = mPool.Create();

            if (entity.mIndex >= mRecords.size())
                mRecords.resize(entity.mIndex + 1);

            // no defer needed
            mRecords[entity.mIndex] = EntityRecord{ mEmptyArchetype, mEmptyArchetype->AddRow(entity) };
            detail::HierarchyMutator::AddRoot(mHierarchy, entity);

            return entity;
        }

        template <typename T>
        void SetComponentHooks(ComponentHooks hooks)
        {
            MIR_ASSERT_COMPONENT(T);
            const std::size_t bit = ComponentBit<T>();

            if (bit >= mComponentHooks.size())
                mComponentHooks.resize(bit + 1);

            HookSlot &slot = mComponentHooks[bit];
            MIR_ASSERT(!slot.hooks.onAdd && !slot.hooks.onRemove,
                "World::SetComponentHooks: {} already has hooks", TrimTypeName<T>());
            slot.hooks = std::move(hooks);
            slot.name = TypeIdOf<T>().name;
        }

        template <typename T>
        void ClearComponentHooks()
        {
            const std::size_t bit = ComponentBit<T>();
            if (bit < mComponentHooks.size())
                mComponentHooks[bit].hooks = ComponentHooks{};
        }

        // reset record, remove from both storages, remove from pool
        void DestroyEntity(Entity entity)
        {
            MIR_ASSERT(mPool.IsAlive(entity), "World::DestroyEntity: entity is not alive");
            AssertSafeToMutate("DestroyEntity");

            // a hook destroyed it again, e.g. two hooks destroying each other's entity;
            // the outer call finishes it
            if (mRecords[entity.mIndex].destroying)
            {
                MIR_LOG_WARN("World::DestroyEntity: entity {} is already being destroyed; a hook "
                    "destroyed it again, ignored",
                    entity.mIndex);
                return;
            }

            mRecords[entity.mIndex].destroying = true;
            const DestroyingMark mark{ mRecords, entity.mIndex };

            // children first, so the destroy is bottom-up. Each stays linked until its own
            // Remove, so its hooks still read its parent. A copy, since each Remove edits the list
            const std::span<const Entity> linked = mHierarchy.ChildrenOf(entity);
            const std::vector<Entity> children(linked.begin(), linked.end());
            for (const Entity child : children)
            {
                MIR_ASSERT(mPool.IsAlive(child), "World::DestroyEntity: dead entity left in the scene graph");

                if (mPool.IsAlive(child))
                    DestroyEntity(child);
            }

            // in bit order, and the row drops once after the last, so every hook reads all the data
            const Signature signature = mRecords[entity.mIndex].archetype->GetSignature();
            for (std::size_t bit = 0; bit < mComponentHooks.size(); ++bit)
            {
                if (signature.test(bit))
                    RunOnRemove(entity, bit);
            }

            detail::HierarchyMutator::Remove(mHierarchy, entity);

            const EntityRecord record = mRecords[entity.mIndex];
            RemoveRow(*record.archetype, record.row);

            mRecords[entity.mIndex] = EntityRecord{};
            mPool.Destroy(entity);
        }

        bool IsAlive(Entity entity) const { return mPool.IsAlive(entity); }

        // accessing the scene graph
        mir::HierarchyIndex &HierarchyIndex() { return mHierarchy; }
        const mir::HierarchyIndex &HierarchyIndex() const { return mHierarchy; }

        template <typename Fn>
        void ForEachAliveEntity(Fn &&fn) const
        {
            for (uint32_t i = 0; i < static_cast<uint32_t>(mRecords.size()); ++i)
            {
                const Entity entity{ i, mPool.GenerationOf(i) };
                if (mPool.IsAlive(entity))
                    fn(entity);
            }
        }

        // Structual changes forbidden while Query is iterating
        bool IsIterating() const { return mQueryIterationDepth > 0; }

        // GetComponent table of entity
        const Archetype *ArchetypeOf(Entity entity) const
        {
            MIR_ASSERT(mPool.IsAlive(entity), "World::ArchetypeOf: entity is not alive");
            return mRecords[entity.mIndex].archetype;
        }

        std::size_t ArchetypeCount() const { return mArchetypes.size(); }

        // archetype generation
        // bumps every time new archetype component is added
        std::size_t Generation() const { return mArchetypeGeneration; }

    #pragma region Typed operations
        template <typename T>
        bool Has(Entity entity) const
        {
            MIR_ASSERT(mPool.IsAlive(entity), "World::Has: entity is not alive");
            return mRecords[entity.mIndex].archetype->GetSignature().test(ComponentBit<T>());
        }

        template <typename T>
        T *GetComponent(Entity entity)
        {
            static_assert(!kIsTagComponent<T>,
                "World::GetComponent: T is a tag - it has no fields, so there is nothing to "
                "read. Use Has<T> instead.");

            if (!mPool.IsAlive(entity))
                return nullptr;

            const EntityRecord &record = mRecords[entity.mIndex];
            ComponentColumn *column = record.archetype->FindColumn(TypeIdOf<T>());
            return column ? static_cast<T *>(column->GetComponent(record.row)) : nullptr;
        }

        template <typename T>
        const T *GetComponent(Entity entity) const
        {
            static_assert(!kIsTagComponent<T>,
                "World::GetComponent: T is a tag - it has no fields, so there is nothing to "
                "read. Use Has<T> instead.");

            if (!mPool.IsAlive(entity))
                return nullptr;

            const EntityRecord &record = mRecords[entity.mIndex];
            const ComponentColumn *column = record.archetype->FindColumn(TypeIdOf<T>());
            return column ? static_cast<const T *>(column->GetComponent(record.row)) : nullptr;
        }

        // use add tag if T is an empty struct
        template <typename T>
        T &AddComponent(Entity entity, const T &value)
        {
            MIR_ASSERT_COMPONENT(T);
            static_assert(!kIsTagComponent<T>,
                "World::AddComponent: T is a tag - there is no value to store and no reference "
                "to return. Use AddTag<T>(entity).");

            MIR_ASSERT(mPool.IsAlive(entity), "World::AddComponent: entity is not alive");
            if (T *existing = GetComponent<T>(entity))
                return *existing;

            AssertSafeToMutate("AddComponent");

            return AddTableComponent(entity, value);
        }

        template <typename T>
        void AddTag(Entity entity)
        {
            // no column is made for tag but saved inside Archetype's signature
            MIR_ASSERT_COMPONENT(T);
            static_assert(kIsTagComponent<T>,
                "World::AddTag: T has fields, so it needs a value. Use AddComponent<T>.");

            MIR_ASSERT(mPool.IsAlive(entity), "World::AddTag: entity is not alive");
            MIR_ASSERT(!Has<T>(entity), "World::AddTag: entity already has this tag");
            AssertSafeToMutate("AddTag");

            AddTableComponentRaw(entity, TypeIdOf<T>(), 0, 0, nullptr);
        }

        template <typename T>
        void RemoveComponent(Entity entity)
        {
            MIR_ASSERT(mPool.IsAlive(entity), "World::RemoveComponent: entity is not alive");
            MIR_ASSERT(Has<T>(entity), "World::RemoveComponent: entity does not have this component");
            AssertSafeToMutate("RemoveComponent");

            RemoveTableComponent<T>(entity);
        }
    #pragma endregion

    #pragma region Type-Erased Operations

        void *GetRaw(Entity entity, TypeId type)
        {
            if (!mPool.IsAlive(entity))
                return nullptr;

            const EntityRecord &record = mRecords[entity.mIndex];
            ComponentColumn *column = record.archetype->FindColumn(type);
            return column ? column->GetComponent(record.row) : nullptr;
        }

        const void *GetRaw(Entity entity, TypeId type) const
        {
            if (!mPool.IsAlive(entity))
                return nullptr;

            const EntityRecord &record = mRecords[entity.mIndex];
            const ComponentColumn *column = record.archetype->FindColumn(type);
            return column ? column->GetComponent(record.row) : nullptr;
        }

        bool HasRaw(Entity entity, TypeId type) const
        {
            return mPool.IsAlive(entity) && mRecords[entity.mIndex].archetype->GetSignature().test(ComponentBitOf(type));
        }

        // only to be called by ComponentRegistry
        void *AddRaw(Entity entity, TypeId type, uint32_t size, uint32_t align, const void *value)
        {
            MIR_ASSERT(mPool.IsAlive(entity), "World::AddRaw: entity is not alive");
            MIR_ASSERT(!HasRaw(entity, type), "World::AddRaw: entity already has {}", type.name);
            AssertSafeToMutate("AddRaw");

            return AddTableComponentRaw(entity, type, size, align, value);
        }

        // only to be called by ComponentRegistry
        void RemoveRaw(Entity entity, TypeId type)
        {
            MIR_ASSERT(mPool.IsAlive(entity), "World::RemoveRaw: entity is not alive");
            MIR_ASSERT(HasRaw(entity, type), "World::RemoveRaw: entity does not have {}", type.name);
            AssertSafeToMutate("RemoveRaw");

            RemoveTableComponentRaw(entity, type);
        }
    #pragma endregion

        template <typename T, typename... Args>
        T &EmplaceResource(Args &&...args)
        {
            static_assert(std::is_same_v<T, std::remove_cvref_t<T>>,
                "World::EmplaceResource: T must be a plain value type, not a reference or cv-qualified");

            auto holder = std::make_unique<detail::ResourceHolder<T>>(std::forward<Args>(args)...);
            T &value = holder->mValue;

            mResources[detail::ResourceKeyOf<T>()] = std::move(holder);
            return value;
        }

        // The resource, or nullptr when none has been emplaced.
        template <typename T>
        T *TryResource()
        {
            const auto it = mResources.find(detail::ResourceKeyOf<T>());
            if (it == mResources.end())
                return nullptr;

            using Bare = std::remove_cvref_t<T>;
            return &static_cast<detail::ResourceHolder<Bare> *>(it->second.get())->mValue;
        }

        template <typename T>
        const T *TryResource() const
        {
            const auto it = mResources.find(detail::ResourceKeyOf<T>());
            if (it == mResources.end())
                return nullptr;

            using Bare = std::remove_cvref_t<T>;
            return &static_cast<const detail::ResourceHolder<Bare> *>(it->second.get())->mValue;
        }

        template <typename T>
        T &Resource()
        {
            T *value = TryResource<T>();

            MIR_CHECK(value != nullptr, "World::Resource: no {} has been emplaced", TrimTypeName<T>());
            return *value;
        }

        template <typename T>
        const T &Resource() const
        {
            const T *value = TryResource<T>();
            MIR_CHECK(value != nullptr, "World::Resource: no {} has been emplaced", TrimTypeName<T>());
            return *value;
        }

        template <typename T>
        bool HasResource() const
        {
            return mResources.find(detail::ResourceKeyOf<T>()) != mResources.end();
        }

        // Destroys the resource. True if there was one.
        template <typename T>
        bool RemoveResource()
        {
            return mResources.erase(detail::ResourceKeyOf<T>()) != 0;
        }

        template <typename... Ts, typename... Filters>
        Query<Ts...> &GetOrCreateQuery(Filters... filters);

        template <typename... Ts, typename Fn>
        void ForEach(Fn &&cb);

    protected:
        template <typename...>
        friend class Query;
        friend class detail::ArchetypeMatcher;
        friend struct detail::QueryIterationGuard;

        const std::unordered_map<Signature, std::unique_ptr<Archetype>> &Archetypes() const { return mArchetypes; }

        void BeginQueryIteration() { ++mQueryIterationDepth; }

        void EndQueryIteration()
        {
            MIR_ASSERT(mQueryIterationDepth > 0, "World::EndQueryIteration: not iterating");
            --mQueryIterationDepth;
        }

        // place in every function that induces structual change
        void AssertSafeToMutate([[maybe_unused]] const char *what) const
        {
            MIR_ASSERT(mQueryIterationDepth == 0,
                "World::{}: structural change while a Query is iterating. Record it into a "
                "CommandBuffer instead - see World::IsIterating",
                what);
        }

        void *AddTableComponentRaw(Entity entity, TypeId type, uint32_t size, uint32_t align, const void *value)
        {
            // get archetype
            const EntityRecord from = mRecords[entity.mIndex];

            Signature signature = from.archetype->GetSignature();
            // signature if added target component
            signature.set(ComponentBitOf(type));

            // tags only live in signature of archetype, no column creation
            Archetype &target = size == 0
                ? GetOrCreateAdded(signature, *from.archetype)
                : GetOrCreateAdded(signature, *from.archetype, ComponentColumn(type, size, align));

            const uint32_t row = target.AddRow(entity);
            CopySharedColumns(*from.archetype, from.row, target, row);

            void *slot = nullptr;
            if (size != 0)
            {
                ComponentColumn *column = target.FindColumn(type);
                MIR_CHECK(column != nullptr,
                    "World::AddTableComponentRaw: \"{}\" was registered with size {}, but the target "
                    "archetype has no column for it - it was added as a tag elsewhere.",
                    type.name, size);

                slot = column->GetComponent(row);
                std::memcpy(slot, value, size);
            }

            RemoveRow(*from.archetype, from.row);
            mRecords[entity.mIndex] = EntityRecord{ &target, row };

            const ComponentHooks *hooks = FindHooks(type);
            if (hooks == nullptr || !hooks->onAdd)
                return slot;

            hooks->onAdd(*this, entity);

            // the hook may have moved the entity to another table
            MIR_ASSERT(mPool.IsAlive(entity) && HasRaw(entity, type),
                "World: the OnAdd hook of {} removed it or destroyed its entity", type.name);
            return GetRaw(entity, type);
        }

        void RemoveTableComponentRaw(Entity entity, TypeId type)
        {
            RunOnRemove(entity, ComponentBitOf(type));

            // move entityset to different table
            const EntityRecord from = mRecords[entity.mIndex];
            Signature signature = from.archetype->GetSignature();
            // signature if removed target component
            signature.reset(ComponentBitOf(type));

            Archetype &target = GetOrCreateRemoved(signature, *from.archetype, type);
            const uint32_t row = target.AddRow(entity);

            CopySharedColumns(*from.archetype, from.row, target, row);

            RemoveRow(*from.archetype, from.row);
            mRecords[entity.mIndex] = EntityRecord{ &target, row };
        }

        template <typename T>
        T &AddTableComponent(Entity entity, const T &value)
        {
            // move entityset to different table
            return *static_cast<T *>(
                AddTableComponentRaw(entity, TypeIdOf<T>(), sizeof(T), alignof(T), &value));
        }

        template <typename T>
        void RemoveTableComponent(Entity entity)
        {
            RemoveTableComponentRaw(entity, TypeIdOf<T>());
        }

        // null when the type never had hooks set
        const ComponentHooks *FindHooks(TypeId type) const
        {
            const std::size_t bit = ComponentBitOf(type);
            return bit < mComponentHooks.size() ? &mComponentHooks[bit].hooks : nullptr;
        }

        // clears the destroying mark on every exit, a throwing hook included.
        // by index, since hooks may create entities and grow mRecords
        struct DestroyingMark
        {
            std::vector<EntityRecord> &records;
            uint32_t index;

            ~DestroyingMark() { records[index].destroying = false; }
        };

        void RunOnRemove(Entity entity, std::size_t bit)
        {
            if (bit >= mComponentHooks.size() || !mComponentHooks[bit].hooks.onRemove)
                return;

            [[maybe_unused]] const Archetype *before = mRecords[entity.mIndex].archetype;
            mComponentHooks[bit].hooks.onRemove(*this, entity);

            // other entities may go, which can shift this entity's row but not its table
            MIR_ASSERT(mPool.IsAlive(entity) && mRecords[entity.mIndex].archetype == before,
                "World: the OnRemove hook of {} changed its own entity. Record the change "
                "into a CommandBuffer instead",
                mComponentHooks[bit].name);
        }

        // like merging in merge sort
        static void CopySharedColumns(Archetype &from, uint32_t fromRow, Archetype &to, uint32_t toRow)
        {
            std::vector<ComponentColumn> &fromColumns = from.Columns();
            std::vector<ComponentColumn> &toColumns = to.Columns();

            std::size_t i = 0;
            std::size_t j = 0;
            while (i < fromColumns.size() && j < toColumns.size())
            {
                const uint32_t fromSeq = fromColumns[i].Type().seq;
                const uint32_t toSeq = toColumns[j].Type().seq;

                if (fromSeq == toSeq)
                {
                    std::memcpy(toColumns[j].GetComponent(toRow), fromColumns[i].GetComponent(fromRow), fromColumns[i].ElementSize());
                    ++i;
                    ++j;
                }
                else if (fromSeq < toSeq)
                    ++i;
                else
                    ++j;
            }
        }

        Archetype &Insert(const Signature &signature, std::unique_ptr<Archetype> archetype)
        {
            ++mArchetypeGeneration;
            return *mArchetypes.emplace(signature, std::move(archetype)).first->second;
        }

        // return existing or create empty table of signature
        Archetype &GetOrCreateAdded(const Signature &signature, const Archetype &source, ComponentColumn added)
        {
            auto it = mArchetypes.find(signature);

            // table with sig exists
            if (it != mArchetypes.end())
                return *it->second;

            // make new table
            auto archetype = std::make_unique<Archetype>(signature);
            // create columns of same type as original
            for (const ComponentColumn &column : source.Columns())
                archetype->AddColumn(column.CloneEmpty());
            archetype->AddColumn(std::move(added));

            return Insert(signature, std::move(archetype));
        }

        // The same move for a tag: the new signature differs by one bit, and
        // the column set does not differ at all.
        Archetype &GetOrCreateAdded(const Signature &signature, const Archetype &source)
        {
            auto it = mArchetypes.find(signature);
            if (it != mArchetypes.end())
                return *it->second;

            auto archetype = std::make_unique<Archetype>(signature);
            for (const ComponentColumn &column : source.Columns())
                archetype->AddColumn(column.CloneEmpty());

            return Insert(signature, std::move(archetype));
        }

        // return existing or create empty table of signature
        Archetype &GetOrCreateRemoved(const Signature &signature, const Archetype &source, TypeId removed)
        {
            auto it = mArchetypes.find(signature);
            if (it != mArchetypes.end())
                return *it->second;

            // make new table, skip the target column
            auto archetype = std::make_unique<Archetype>(signature);
            for (const ComponentColumn &column : source.Columns())
            {
                if (column.Type().seq != removed.seq)
                    archetype->AddColumn(column.CloneEmpty());
            }

            return Insert(signature, std::move(archetype));
        }

        Archetype &GetOrCreateArchetype(const Signature &signature)
        {
            auto it = mArchetypes.find(signature);
            if (it != mArchetypes.end())
                return *it->second;

            return Insert(signature, std::make_unique<Archetype>(signature));
        }

        // drops a row and repairs the record of whoever got swapped into it
        void RemoveRow(Archetype &archetype, uint32_t row)
        {
            const Entity moved = archetype.SwapRemoveRow(row);
            if (!moved.IsNull())
                mRecords[moved.mIndex].row = row;
        }

        EntityPool mPool;
        std::vector<EntityRecord> mRecords; // indexed by Entity::mIndex
        std::unordered_map<Signature, std::unique_ptr<Archetype>> mArchetypes;
        Archetype *mEmptyArchetype = nullptr;
        std::unordered_map<uint32_t, std::unique_ptr<detail::IQuery>> mQueries; // by detail::QueryKeyOf
        // the name is kept for assert messages, since a tag has no column to read it from
        struct HookSlot
        {
            ComponentHooks hooks;
            std::string_view name;
        };
        std::vector<HookSlot> mComponentHooks; // by component bit
        mir::HierarchyIndex mHierarchy;

        // intentionally declared last as it must be destroyed first.
        std::unordered_map<uint32_t, std::unique_ptr<detail::IResource>> mResources; // by detail::ResourceIdOf
        std::size_t mArchetypeGeneration = 0;
        uint32_t mQueryIterationDepth = 0; // depth for nested iteration checking
    };
}

// Template complications
#include "Query.h"
