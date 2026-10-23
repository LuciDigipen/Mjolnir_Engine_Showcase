/**
 * @file AuthoredEdit.tests.cpp
 * @author Jackey Cen
 * @brief Tests for editor/AuthoredEdit.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/AuthoredEdit.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/ComponentRegistry.h>
#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using mir::AuthoredAction;
    using mir::AuthoredComponentState;
    using mir::AuthoredEntry;
    using mir::AuthoredFieldState;
    using mir::AuthoredKey;
    using mir::AuthoringWrite;
    using mir::CapturedSubtree;
    using mir::ComponentOps;
    using mir::ComponentRegistry;
    using mir::Entity;
    using mir::FieldKind;
    using mir::FieldValue;
    using mir::History;
    using mir::kNullEntity;
    using mir::RuntimeFieldDecl;
    using mir::World;

    class FakeBackend final : public mir::IAuthoringBackend
    {
    public:
        std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) override
        {
            std::vector<AuthoredEntry> entries;
            for (const AuthoredKey &key : keys)
                entries.push_back(AuthoredEntry{ key, At(key) });
            return entries;
        }

        AuthoringWrite Write(std::span<const AuthoredEntry> entries) override
        {
            mWritten.clear();
            for (const AuthoredEntry &entry : entries)
            {
                std::erase_if(mValues, [&entry](const auto &stored) { return stored.first == entry.mKey; });
                if (entry.mValue)
                    mValues.emplace_back(entry.mKey, *entry.mValue);
                mWritten.push_back(entry.mKey);
            }
            ++mWrites;
            return AuthoringWrite{ true, {} };
        }

        void Put(const AuthoredKey &key, std::string value) { mValues.emplace_back(key, std::move(value)); }

        std::optional<std::string> At(const AuthoredKey &key) const
        {
            const auto it = std::find_if(mValues.begin(), mValues.end(),
                [&key](const auto &stored) { return stored.first == key; });
            return it != mValues.end() ? std::optional<std::string>(it->second) : std::nullopt;
        }

        std::vector<AuthoredKey> mWritten;
        int mWrites = 0;

    private:
        std::vector<std::pair<AuthoredKey, std::string>> mValues;
    };

    class FakeView final : public mir::IAuthoredEntities
    {
    public:
        std::optional<AuthoredKey> KeyOf(Entity entity, std::string_view component,
            std::string_view field) override
        {
            if (!mMembers.contains(mir::PackEntity(entity)))
                return std::nullopt;
            return Key(entity, component, field);
        }

        std::optional<AuthoredKey> ComponentKeyOf(Entity entity, std::string_view component) override
        {
            if (!mMembers.contains(mir::PackEntity(entity)))
                return std::nullopt;
            return Key(entity, component, {});
        }

        AuthoredFieldState FieldState(const AuthoredKey &) override { return AuthoredFieldState::Inherited; }

        AuthoredComponentState ComponentState(Entity, std::string_view) override
        {
            return AuthoredComponentState::Inherited;
        }

        std::vector<std::string> RemovedComponents(Entity) override { return {}; }

        std::string EncodeField(FieldKind kind, const void *bytes) override
        {
            if (kind == FieldKind::Float)
                return std::to_string(*static_cast<const float *>(bytes));
            return "null";
        }

        AuthoredAction Revert(const AuthoredKey &key) override
        {
            AuthoredAction action;
            action.mName = "Revert Override";
            action.mEntries.push_back(AuthoredEntry{ key, std::nullopt });
            return action;
        }

        AuthoredAction Apply(const AuthoredKey &key) override
        {
            AuthoredAction action;
            if (mRefuseApply)
            {
                action.mAllowed = false;
                action.mReason = "scene-dependent";
                return action;
            }
            action.mName = "Apply to Prefab";
            action.mEntries.push_back(AuthoredEntry{ key, std::string("1") });
            return action;
        }

        std::set<uint64_t> mMembers;
        bool mRefuseApply = false;

    private:
        static AuthoredKey Key(Entity entity, std::string_view component, std::string_view field)
        {
            AuthoredKey key;
            key.mObject.mDocument = mir::DocumentId{ 1, 1 };
            key.mObject.mAddress = { mir::PackEntity(entity) };
            key.mComponent = component;
            key.mField = field;
            return key;
        }
    };

    class NoSceneOps final : public mir::ISceneOps
    {
    public:
        Entity CreateEntity() override { return kNullEntity; }
        Entity Duplicate(Entity) override { return kNullEntity; }
        CapturedSubtree Capture(Entity) override { return {}; }
        std::vector<Entity> Restore(const CapturedSubtree &, Entity, Entity) override { return {}; }
        bool Destroy(Entity) override { return false; }
    };

    const ComponentOps &DataOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "amount", FieldKind::Float },
            { "bonus", FieldKind::Float },
        };
        return ComponentRegistry::Instance().RegisterRuntime("AuthoredEditTestData", fields);
    }

    const ComponentOps &ExtraOps()
    {
        static const RuntimeFieldDecl fields[] = {
            { "value", FieldKind::Float },
        };
        return ComponentRegistry::Instance().RegisterRuntime("AuthoredEditTestExtra", fields);
    }

    FieldValue Value(uint32_t field, float value)
    {
        FieldValue out;
        out.mField = field;
        std::memcpy(out.mBytes, &value, sizeof(value));
        return out;
    }

    struct Fixture
    {
        World mWorld;
        FakeBackend mBackend;
        FakeView mView;
        NoSceneOps mScene;
        History mHistory;
        Entity mEntity = kNullEntity;

        Fixture()
        {
            mir::RegisterCoreComponents();
            DataOps();
            ExtraOps();
            mEntity = mWorld.CreateEntity();
            DataOps().AddDefault(mWorld, mEntity);
            mHistory.SetAuthoringBackend(&mBackend);
        }

        void Run(std::unique_ptr<mir::ICommand> command)
        {
            mHistory.Submit(std::move(command));
            mHistory.Flush(mWorld, mScene);
        }
    };
}

TEST_CASE("An ordinary entity field edit keeps the World command", "[editor][history][authored-edit]")
{
    Fixture fixture;
    fixture.Run(mir::MakeSetFieldsEditCommand(fixture.mEntity, DataOps(), { Value(0, 3.0f) }, "", &fixture.mView));

    CHECK(fixture.mHistory.StepCount() == 1);
    CHECK(fixture.mBackend.mWrites == 0);
    float amount = 0.0f;
    DataOps().FindField("amount")->Read(DataOps().GetComponent(fixture.mWorld, fixture.mEntity), &amount);
    CHECK(amount == 3.0f);
}

TEST_CASE("A member field edit writes one authored entry and merges", "[editor][history][authored-edit]")
{
    Fixture fixture;
    fixture.mView.mMembers.insert(mir::PackEntity(fixture.mEntity));

    fixture.Run(mir::MakeSetFieldsEditCommand(fixture.mEntity, DataOps(), { Value(0, 1.0f) }, "",
        &fixture.mView));
    fixture.Run(mir::MakeSetFieldsEditCommand(fixture.mEntity, DataOps(), { Value(0, 2.0f) }, "",
        &fixture.mView));
    fixture.Run(mir::MakeSetFieldsEditCommand(fixture.mEntity, DataOps(), { Value(0, 3.0f) }, "",
        &fixture.mView));

    CHECK(fixture.mHistory.StepCount() == 1);
    REQUIRE(fixture.mBackend.mWritten.size() == 1);
    CHECK(fixture.mBackend.mWritten.front().mComponent == "AuthoredEditTestData");
    CHECK(fixture.mBackend.mWritten.front().mField == "amount");
    CHECK(fixture.mBackend.At(fixture.mBackend.mWritten.front()) == "3.000000");
}

TEST_CASE("Add and remove component route to authored entries for a member", "[editor][history][authored-edit]")
{
    Fixture fixture;
    fixture.mView.mMembers.insert(mir::PackEntity(fixture.mEntity));

    fixture.Run(mir::MakeAddComponentEditCommand(fixture.mEntity, ExtraOps(), {}, &fixture.mView));
    CHECK(fixture.mHistory.StepCount() == 1);
    CHECK(fixture.mBackend.mWrites == 1);
    REQUIRE_FALSE(fixture.mBackend.mWritten.empty());
    CHECK(fixture.mBackend.mWritten.front().mComponent == "AuthoredEditTestExtra");
    CHECK_FALSE(ExtraOps().Has(fixture.mWorld, fixture.mEntity));

    fixture.Run(mir::MakeRemoveComponentEditCommand(fixture.mEntity, DataOps(), &fixture.mView));
    CHECK(fixture.mHistory.StepCount() == 2);
    CHECK(fixture.mBackend.mWritten.size() == 1);
    CHECK(fixture.mBackend.mWritten.front().mComponent == "AuthoredEditTestData");
    CHECK(fixture.mBackend.At(fixture.mBackend.mWritten.front()) == "null");
    CHECK(DataOps().Has(fixture.mWorld, fixture.mEntity));
}

TEST_CASE("Revert submits one command and a refused Apply submits none", "[editor][history][authored-edit]")
{
    Fixture fixture;
    fixture.mView.mMembers.insert(mir::PackEntity(fixture.mEntity));

    const AuthoredKey key = *fixture.mView.KeyOf(fixture.mEntity, "AuthoredEditTestData", "amount");
    fixture.mBackend.Put(key, "5");
    fixture.Run(mir::MakeRevertAuthoredCommand(fixture.mView, key));
    CHECK(fixture.mHistory.StepCount() == 1);
    CHECK_FALSE(fixture.mBackend.At(key).has_value());

    fixture.mView.mRefuseApply = true;
    CHECK(mir::MakeApplyAuthoredCommand(fixture.mView, key) == nullptr);
    CHECK(fixture.mHistory.StepCount() == 1);
}
