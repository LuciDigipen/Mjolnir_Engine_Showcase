/**
 * @file ScriptRef.h
 * @author Rahul Nair
 * @brief Which script an entity runs, and whether OnStart has fired for it
 *        yet.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/ecs/ComponentAsserts.h>
#include <core/ecs/ComponentFields.h>

#include <cstddef>
#include <cstdint>

namespace mir
{
    struct ScriptRef
    {
        int32_t instanceRef = -1; // IScriptHost-owned handle, -1 = none
        bool started = false;     // OnStart has run for this instance

        // Name the script was registered under (IScriptHost::LoadScriptSource),
        // not a runtime handle - unlike instanceRef, this is what a scene file
        // actually saves: instanceRef is only meaningful within one run's live
        // ScriptHost, so LoadScene re-derives it from this name instead of
        // trying to restore the handle itself (see ScriptResolveSystem).
        char scriptName[kFieldStringCapacity] = {};

        // the scriptName instanceRef was made from, so a rename is seen. Transient
        // like instanceRef: never serialized, snapshotted or copied
        char resolvedName[kFieldStringCapacity] = {};
    };

    MIR_ASSERT_COMPONENT(ScriptRef);

    // Fields for scripts, inspectors, and scene serialization. instanceRef, started
    // and resolvedName are deliberately left out - they're this run's live state, not
    // anything a scene file should round-trip.
    inline constexpr FieldDesc kScriptRefFields[] = {
        {"scriptName", FieldKind::String, offsetof(ScriptRef, scriptName)},
    };

    inline constexpr TransientField kScriptRefTransient[] = {
        { offsetof(ScriptRef, instanceRef), sizeof(ScriptRef::instanceRef) },
        { offsetof(ScriptRef, started), sizeof(ScriptRef::started) },
        { offsetof(ScriptRef, resolvedName), sizeof(ScriptRef::resolvedName) },
    };
}
