/**
 * @file ActiveCameraEdit.tests.cpp
 * @author Quy Dong Ta
 * @brief Turning a Camera active from the Inspector: one undo step that also clears the other cameras.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "EditingFixture.h"

#include <editor/history/ActiveCameraEdit.h>

#include <core/ecs/TypeId.h>
#include <renderer/ComponentRegistration.h>
#include <renderer/components/Camera.h>

#include <cstring>
#include <string_view>

namespace
{
    using mir::Camera;
    using mir::FieldValue;

    const ComponentOps &CameraOps()
    {
        static const ComponentOps &ops = (mir::RegisterRendererComponents(),
            *ComponentRegistry::Instance().FindBySeq(mir::TypeIdOf<Camera>().seq));
        return ops;
    }

    template <typename T>
    FieldValue ValueOf(std::string_view field, const T &value)
    {
        FieldValue out;
        out.mField = static_cast<uint32_t>(CameraOps().FindField(field) - CameraOps().mFields.data());
        std::memcpy(out.mBytes, &value, sizeof(T));
        return out;
    }

    bool Active(Editing &e, Entity camera) { return e.mWorld.GetComponent<Camera>(camera)->mActive; }

    std::unique_ptr<ICommand> Edit(Editing &e, Entity camera, FieldValue value)
    {
        return mir::MakeFieldEditCommand(e.mWorld, camera, CameraOps(), { value }, "", nullptr);
    }
}

TEST_CASE("ActiveCameraEdit: turning a Camera active clears the others, and undo restores them", "[editor][history]")
{
    Editing e;
    const Entity a = e.Add("a");
    const Entity b = e.Add("b");
    const Entity c = e.Add("c");
    e.mWorld.AddComponent<Camera>(a, Camera{ .mActive = true });
    e.mWorld.AddComponent<Camera>(b, Camera{});
    e.mWorld.AddComponent<Camera>(c, Camera{});

    e.Run(Edit(e, b, ValueOf("active", true)));
    CHECK_FALSE(Active(e, a));
    CHECK(Active(e, b));
    CHECK_FALSE(Active(e, c));

    e.Undo();
    CHECK(Active(e, a));
    CHECK_FALSE(Active(e, b));
    CHECK_FALSE(Active(e, c));

    e.Redo();
    CHECK_FALSE(Active(e, a));
    CHECK(Active(e, b));
}

TEST_CASE("ActiveCameraEdit: other Camera edits touch only their own camera", "[editor][history]")
{
    Editing e;
    const Entity a = e.Add("a");
    const Entity b = e.Add("b");
    e.mWorld.AddComponent<Camera>(a, Camera{ .mActive = true });
    e.mWorld.AddComponent<Camera>(b, Camera{ .mActive = true });

    e.Run(Edit(e, b, ValueOf("fovYDegrees", 75.0f)));
    CHECK(Active(e, a));
    CHECK(e.mWorld.GetComponent<Camera>(b)->mFovYDegrees == 75.0f);

    e.Run(Edit(e, b, ValueOf("active", false)));
    CHECK(Active(e, a));
    CHECK_FALSE(Active(e, b));

    e.Undo();
    CHECK(Active(e, a));
    CHECK(Active(e, b));
}
