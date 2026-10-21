/**
 * @file Authoring.tests.cpp
 * @author Dong Ta
 * @brief Authored commands across documents, run against a fake backend in place of the Prefab side.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <editor/history/Authoring.h>
#include <editor/history/History.h>
#include <editor/history/SceneOps.h>

#include <core/ecs/World.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
    using mir::AuthoredEntry;
    using mir::AuthoredKey;
    using mir::AuthoredObject;
    using mir::AuthoringWrite;
    using mir::CapturedSubtree;
    using mir::DocumentId;
    using mir::Entity;
    using mir::History;
    using mir::kNullEntity;
    using mir::kSceneDocument;
    using mir::MakeAuthoredCommand;
    using mir::World;

    // authored entries per key, as the Prefab side holds them in its documents
    class FakeBackend final : public mir::IAuthoringBackend
    {
    public:
        void AddObject(const AuthoredObject &object) { mObjects.push_back(object); }

        // the overridden value, or empty when the key inherits
        std::optional<std::string> At(const AuthoredKey &key) const
        {
            const auto it = Find(key);
            return it != mEntries.end() ? std::optional<std::string>(it->second) : std::nullopt;
        }

        // what a reader of the object sees: the override, else the inherited default
        std::optional<std::string> Resolved(const AuthoredKey &key) const
        {
            if (const std::optional<std::string> value = At(key))
                return value;
            const auto it = std::find_if(mInherited.begin(), mInherited.end(),
                [&key](const auto &entry) { return entry.first == key; });
            return it != mInherited.end() ? std::optional<std::string>(it->second) : std::nullopt;
        }

        void Inherit(const AuthoredKey &key, std::string value) { mInherited.emplace_back(key, std::move(value)); }
        void Put(const AuthoredKey &key, std::string value) { Store(AuthoredEntry{ key, std::move(value) }); }

        std::optional<std::vector<AuthoredEntry>> Read(std::span<const AuthoredKey> keys) override
        {
            std::vector<AuthoredEntry> entries;
            for (const AuthoredKey &key : keys)
            {
                if (!HasObject(key.mObject))
                    return std::nullopt;
                entries.push_back(AuthoredEntry{ key, At(key) });
            }
            return entries;
        }

        AuthoringWrite Write(std::span<const AuthoredEntry> entries) override
        {
            for (const AuthoredEntry &entry : entries)
            {
                const DocumentId document = entry.mKey.mObject.mDocument;
                if (!HasObject(entry.mKey.mObject) || entry.mValue == "invalid" ||
                    std::find(mLocked.begin(), mLocked.end(), document) != mLocked.end())
                    return {};
            }

            mWrittenKeys.clear();
            for (const AuthoredEntry &entry : entries)
            {
                Store(entry);
                mWrittenKeys.push_back(entry.mKey);
            }
            ++mWrites;
            return AuthoringWrite{ true, mReportMaintained };
        }

        std::vector<DocumentId> mLocked;            // documents that refuse every write
        std::vector<DocumentId> mReportMaintained;  // what a write says it cleaned up on its own
        std::vector<AuthoredKey> mWrittenKeys;      // the last write's keys
        int mWrites = 0;
        std::string mTransient = "mesh handle 7";   // runtime state, never authored

    private:
        bool HasObject(const AuthoredObject &object) const
        {
            return std::find(mObjects.begin(), mObjects.end(), object) != mObjects.end();
        }

        std::vector<std::pair<AuthoredKey, std::string>>::const_iterator Find(const AuthoredKey &key) const
        {
            return std::find_if(mEntries.begin(), mEntries.end(), [&key](const auto &entry) { return entry.first == key; });
        }

        void Store(const AuthoredEntry &entry)
        {
            std::erase_if(mEntries, [&entry](const auto &stored) { return stored.first == entry.mKey; });
            if (entry.mValue)
                mEntries.emplace_back(entry.mKey, *entry.mValue);
        }

        std::vector<AuthoredObject> mObjects;
        std::vector<std::pair<AuthoredKey, std::string>> mEntries;
        std::vector<std::pair<AuthoredKey, std::string>> mInherited;
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

    // a scene command from #203 or #204, as far as save state goes
    class SceneEdit final : public mir::ICommand
    {
    public:
        bool Do(mir::CommandContext &) override { return true; }
        bool Undo(mir::CommandContext &) override { return true; }
        std::size_t Size() const override { return 16; }
        std::string Name() const override { return "Scene Edit"; }
        void CollectIds(std::vector<mir::HistoryId> &) const override {}
    };

    // House holds the Door Prefab twice; the scene has one House instance with root StableId 100
    const DocumentId kDoor{ 1, 1 };
    const DocumentId kHouse{ 2, 2 };
    const AuthoredObject kDoorPanel{ kDoor, { 3 } };
    const AuthoredObject kHouseFrontPanel{ kHouse, { 10, 3 } };
    const AuthoredObject kSceneFrontPanel{ kSceneDocument, { 100, 10, 3 } };
    const AuthoredObject kSceneLamp{ kSceneDocument, { 100, 30 } };
    const AuthoredObject kMissing{ kDoor, { 99 } };

    AuthoredKey Field(const AuthoredObject &object, std::string component, std::string field)
    {
        return AuthoredKey{ object, std::move(component), std::move(field) };
    }

    AuthoredKey Presence(const AuthoredObject &object, std::string component)
    {
        return AuthoredKey{ object, std::move(component), {} };
    }

    AuthoredKey Record(const AuthoredObject &object) { return AuthoredKey{ object, {}, {} }; }

    AuthoredEntry Set(const AuthoredKey &key, std::string value) { return AuthoredEntry{ key, std::move(value) }; }
    AuthoredEntry Inherit(const AuthoredKey &key) { return AuthoredEntry{ key, std::nullopt }; }

    struct Authoring
    {
        World mWorld;
        NoSceneOps mScene;
        FakeBackend mBackend;
        History mHistory;

        Authoring()
        {
            mHistory.SetAuthoringBackend(&mBackend);
            for (const AuthoredObject &object : { kDoorPanel, kHouseFrontPanel, kSceneFrontPanel, kSceneLamp })
                mBackend.AddObject(object);
        }

        void Run(std::unique_ptr<mir::ICommand> command)
        {
            mHistory.Submit(std::move(command));
            Flush();
        }

        void Edit(std::vector<AuthoredEntry> changes, std::vector<AuthoredEntry> maintenance = {})
        {
            Run(MakeAuthoredCommand("Edit", std::move(changes), std::move(maintenance)));
        }

        void Undo()
        {
            mHistory.RequestUndo();
            Flush();
        }

        void Redo()
        {
            mHistory.RequestRedo();
            Flush();
        }

        void Release()
        {
            mHistory.EndStep();
            Flush();
        }

        void Save(DocumentId document) { mHistory.SetUnsaved(document, false); }
        bool Unsaved(DocumentId document) const { return mHistory.IsUnsaved(document); }
        void Flush() { mHistory.Flush(mWorld, mScene); }
    };

    const AuthoredKey kIntensity = Field(kDoorPanel, "Light", "intensity");
}

TEST_CASE("An override, its undo back to inherited, and its redo", "[editor][history][authoring]")
{
    Authoring a;
    a.Edit({ Set(kIntensity, "2") });

    CHECK(a.mBackend.At(kIntensity) == "2");
    CHECK(a.mHistory.StepCount() == 1);

    a.Undo();
    CHECK_FALSE(a.mBackend.At(kIntensity).has_value());

    a.Redo();
    CHECK(a.mBackend.At(kIntensity) == "2");
}

TEST_CASE("An override equal to the inherited value is still an override", "[editor][history][authoring]")
{
    Authoring a;
    a.mBackend.Inherit(kIntensity, "5");

    SECTION("adding it is a step, and undo makes the field inherit again")
    {
        a.Edit({ Set(kIntensity, "5") });
        CHECK(a.mHistory.StepCount() == 1);
        CHECK(a.mBackend.At(kIntensity) == "5");

        a.Undo();
        CHECK_FALSE(a.mBackend.At(kIntensity).has_value());
        CHECK(a.mBackend.Resolved(kIntensity) == "5");
    }
    SECTION("removing it is a step, and undo puts the override back")
    {
        a.mBackend.Put(kIntensity, "5");
        a.Edit({ Inherit(kIntensity) });
        CHECK(a.mHistory.StepCount() == 1);
        CHECK_FALSE(a.mBackend.At(kIntensity).has_value());

        a.Undo();
        CHECK(a.mBackend.At(kIntensity) == "5");
    }
    SECTION("writing the override it already has is no step")
    {
        a.mBackend.Put(kIntensity, "5");
        a.Edit({ Set(kIntensity, "5") });
        CHECK(a.mHistory.StepCount() == 0);
    }
}

TEST_CASE("Only the fields an edit names are captured", "[editor][history][authoring]")
{
    Authoring a;
    const AuthoredKey color = Field(kDoorPanel, "Light", "color");
    const AuthoredKey range = Field(kDoorPanel, "Light", "range");
    a.mBackend.Put(color, "red");

    a.Edit({ Set(kIntensity, "2") });
    CHECK(a.mBackend.mWrittenKeys == std::vector<AuthoredKey>{ kIntensity });

    a.Undo();
    CHECK(a.mBackend.mWrittenKeys == std::vector<AuthoredKey>{ kIntensity });

    CHECK(a.mBackend.At(color) == "red");
    CHECK_FALSE(a.mBackend.At(range).has_value());
    CHECK(a.mBackend.mTransient == "mesh handle 7");
}

TEST_CASE("Component states undo back to inherited", "[editor][history][authoring]")
{
    Authoring a;

    SECTION("a locally added component, with its fields, is one step")
    {
        a.Edit({ Set(Presence(kDoorPanel, "Light"), "added"), Set(kIntensity, "3") });
        CHECK(a.mHistory.StepCount() == 1);

        a.Undo();
        CHECK_FALSE(a.mBackend.At(Presence(kDoorPanel, "Light")).has_value());
        CHECK_FALSE(a.mBackend.At(kIntensity).has_value());
    }
    SECTION("an inherited component removed")
    {
        a.Edit({ Set(Presence(kDoorPanel, "RigidBody"), "removed") });
        CHECK(a.mBackend.At(Presence(kDoorPanel, "RigidBody")) == "removed");

        a.Undo();
        CHECK_FALSE(a.mBackend.At(Presence(kDoorPanel, "RigidBody")).has_value());
    }
}

TEST_CASE("Writes across documents are one step", "[editor][history][authoring]")
{
    // Apply: the scene's override moves up into House, and the scene stops overriding
    Authoring a;
    const AuthoredKey house = Field(kHouseFrontPanel, "Light", "intensity");
    const AuthoredKey scene = Field(kSceneFrontPanel, "Light", "intensity");
    a.mBackend.Put(scene, "4");

    a.Run(MakeAuthoredCommand("Apply", { Set(house, "4"), Inherit(scene) }));
    CHECK(a.mHistory.StepCount() == 1);
    CHECK(a.mHistory.UndoName() == "Apply");
    CHECK(a.mBackend.mWrites == 1);

    a.Undo();
    CHECK_FALSE(a.mBackend.At(house).has_value());
    CHECK(a.mBackend.At(scene) == "4");

    a.Redo();
    CHECK(a.mBackend.At(house) == "4");
    CHECK_FALSE(a.mBackend.At(scene).has_value());
}

TEST_CASE("A failed target or batch applies nothing and leaves the cursor", "[editor][history][authoring]")
{
    Authoring a;
    const AuthoredKey scene = Field(kSceneFrontPanel, "Light", "intensity");

    SECTION("a missing object refuses the whole batch")
    {
        a.Edit({ Set(scene, "4"), Set(Field(kMissing, "Light", "intensity"), "1") });
        CHECK(a.mHistory.StepCount() == 0);
        CHECK(a.mBackend.mWrites == 0);
        CHECK_FALSE(a.mBackend.At(scene).has_value());
    }
    SECTION("an invalid value in one document refuses the other's too")
    {
        a.Edit({ Set(scene, "4"), Set(kIntensity, "invalid") });
        CHECK(a.mHistory.StepCount() == 0);
        CHECK_FALSE(a.mBackend.At(scene).has_value());
    }
    SECTION("an undo the backend refuses stays undoable")
    {
        a.Edit({ Set(scene, "4"), Set(kIntensity, "1") });
        a.mBackend.mLocked = { kDoor };

        a.Undo();
        CHECK(a.mHistory.Cursor() == 1);
        CHECK(a.mBackend.At(scene) == "4");

        a.mBackend.mLocked.clear();
        a.Undo();
        CHECK(a.mHistory.Cursor() == 0);
        CHECK_FALSE(a.mBackend.At(scene).has_value());
    }
    SECTION("a redo the backend refuses stays redoable")
    {
        a.Edit({ Set(scene, "4") });
        a.Undo();
        a.mBackend.mLocked = { kSceneDocument };

        a.Redo();
        CHECK(a.mHistory.Cursor() == 0);
        CHECK_FALSE(a.mBackend.At(scene).has_value());
        CHECK(a.mHistory.CanRedo());
    }
    SECTION("no backend at all")
    {
        a.mHistory.SetAuthoringBackend(nullptr);
        a.Edit({ Set(scene, "4") });
        CHECK(a.mHistory.StepCount() == 0);
    }
}

TEST_CASE("Merges need the same document, object, component and field", "[editor][history][authoring]")
{
    Authoring a;
    a.Edit({ Set(kIntensity, "1") });

    SECTION("the same key folds into one step")
    {
        a.Edit({ Set(kIntensity, "2") });
        a.Edit({ Set(kIntensity, "3") });
        CHECK(a.mHistory.StepCount() == 1);

        a.Undo();
        CHECK_FALSE(a.mBackend.At(kIntensity).has_value());
    }
    SECTION("another document")
    {
        a.Edit({ Set(Field(kHouseFrontPanel, "Light", "intensity"), "2") });
        CHECK(a.mHistory.StepCount() == 2);
    }
    SECTION("another object in the same document")
    {
        a.Edit({ Set(Field(kSceneLamp, "Light", "intensity"), "2") });
        a.Edit({ Set(Field(kSceneFrontPanel, "Light", "intensity"), "2") });
        CHECK(a.mHistory.StepCount() == 3);
    }
    SECTION("another field")
    {
        a.Edit({ Set(Field(kDoorPanel, "Light", "range"), "2") });
        CHECK(a.mHistory.StepCount() == 2);
    }
    SECTION("the same key after a release")
    {
        a.Release();
        a.Edit({ Set(kIntensity, "2") });
        CHECK(a.mHistory.StepCount() == 2);
    }
}

TEST_CASE("Each document has its own save point", "[editor][history][authoring]")
{
    Authoring a;
    const AuthoredKey scene = Field(kSceneFrontPanel, "Light", "intensity");

    a.Edit({ Set(kIntensity, "2") });
    CHECK(a.Unsaved(kDoor));
    CHECK_FALSE(a.Unsaved(kSceneDocument));
    CHECK_FALSE(a.Unsaved(kHouse));

    a.Save(kDoor);
    a.Edit({ Set(scene, "4") });
    CHECK(a.Unsaved(kSceneDocument));
    CHECK_FALSE(a.Unsaved(kDoor));

    a.Undo();
    CHECK_FALSE(a.Unsaved(kSceneDocument));
    CHECK_FALSE(a.Unsaved(kDoor));

    a.Undo();
    CHECK(a.Unsaved(kDoor));

    SECTION("a step across two documents moves both")
    {
        a.Redo();
        a.Edit({ Set(kIntensity, "3"), Set(scene, "4") });
        a.Save(kDoor);
        a.Save(kSceneDocument);

        a.Undo();
        CHECK(a.Unsaved(kDoor));
        CHECK(a.Unsaved(kSceneDocument));

        a.Redo();
        CHECK_FALSE(a.Unsaved(kDoor));
        CHECK_FALSE(a.Unsaved(kSceneDocument));
    }
    SECTION("a scene command from #203 or #204 moves only the scene")
    {
        a.Redo();
        a.Run(std::make_unique<SceneEdit>());
        CHECK(a.Unsaved(kSceneDocument));
        CHECK_FALSE(a.Unsaved(kDoor));
    }
    SECTION("clearing history keeps each document's marker")
    {
        a.mHistory.Clear();
        CHECK(a.Unsaved(kDoor));
        CHECK_FALSE(a.Unsaved(kSceneDocument));
    }
}

TEST_CASE("Cleanup Undo does not reverse keeps its document unsaved", "[editor][history][authoring]")
{
    // deleting Door's panel clears the scene's reference into it, and undo brings back only the panel
    Authoring a;
    const AuthoredKey panel = Record(kDoorPanel);
    const AuthoredKey sceneRef = Field(kSceneLamp, "Switch", "target");
    a.mBackend.Put(panel, "node");
    a.mBackend.Put(sceneRef, "[100,10,3]");
    a.Save(kDoor);
    a.Save(kSceneDocument);

    SECTION("declared with the deletion")
    {
        a.Edit({ Inherit(panel) }, { Set(sceneRef, "null") });
    }
    SECTION("reported by the backend's write")
    {
        a.mBackend.mReportMaintained = { kSceneDocument };
        a.mBackend.Put(sceneRef, "null");
        a.Edit({ Inherit(panel) });
        a.mBackend.mReportMaintained.clear();
    }

    CHECK_FALSE(a.mBackend.At(panel).has_value());
    CHECK(a.mBackend.At(sceneRef) == "null");
    CHECK(a.Unsaved(kDoor));
    CHECK(a.Unsaved(kSceneDocument));

    a.Undo();
    CHECK(a.mBackend.At(panel) == "node");
    CHECK(a.mBackend.At(sceneRef) == "null");
    CHECK_FALSE(a.Unsaved(kDoor));
    CHECK(a.Unsaved(kSceneDocument));

    a.Redo();
    CHECK_FALSE(a.mBackend.At(panel).has_value());
    CHECK(a.mBackend.At(sceneRef) == "null");

    a.Save(kSceneDocument);
    CHECK_FALSE(a.Unsaved(kSceneDocument));

    a.Undo();
    CHECK_FALSE(a.Unsaved(kDoor));
    CHECK_FALSE(a.Unsaved(kSceneDocument));
}
