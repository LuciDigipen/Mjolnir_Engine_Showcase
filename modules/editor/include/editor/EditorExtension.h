/**
 * @file EditorExtension.h
 * @author Jackey Cen
 * @brief The hooks an outside module registers with to add panels, viewport UI and an owned edit mode.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <core/Registration.h>
#include <core/platform/Surface.h>
#include <editor/EditorCamera.h>
#include <editor/history/DocumentId.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

struct ImDrawList;

namespace mir
{
    class History;
    class ISceneOps;
    class World;
    struct EngineModeState;

    // What an extension may read while the editor calls it.
    struct EditorExtensionContext
    {
        const EngineModeState &mMode;
        DocumentId mDocument = kSceneDocument;
        World &mWorld;
        PixelRect mViewport{};
        const EditorCamera &mCamera;
        const RawInputSnapshot &mInput;
        bool mWantsCaptureMouse = false;
        // set for DrawOverlay only
        ImDrawList *mOverlay = nullptr;
        History &mHistory;
    };

    class IEditorExtension
    {
    public:
        virtual ~IEditorExtension() = default;

        // once per DrawLayout, before the viewport picking and gizmo
        virtual void Update(const EditorExtensionContext &context) { (void)context; }

        // window names the first layout docks at the right node, beside Inspector
        virtual std::span<const char *const> WindowNames() const { return {}; }

        // after the built-in panels
        virtual void DrawWindows(const EditorExtensionContext &context) { (void)context; }

        // after the built-in viewport toolbar buttons
        virtual void DrawToolbar(const EditorExtensionContext &context) { (void)context; }

        // true: the editor draws no transform gizmo, reads no W/E/R and picks no entity
        virtual bool OwnsViewport() const { return false; }

        // after the gizmo, into the viewport overlay
        virtual void DrawOverlay(const EditorExtensionContext &context) { (void)context; }

        // Before queued commands or undo/redo may invalidate an active gesture.
        virtual void BeforeHistoryFlush(History &history) { (void)history; }
    };

    // Extensions run in registration order; each Registration removes its extension when it dies.
    class EditorExtensions
    {
    public:
        EditorExtensions() = default;
        EditorExtensions(const EditorExtensions &) = delete;
        EditorExtensions &operator=(const EditorExtensions &) = delete;

        Registration Add(IEditorExtension &extension);

        void Update(const EditorExtensionContext &context) const;
        void DrawWindows(const EditorExtensionContext &context) const;
        void DrawToolbar(const EditorExtensionContext &context) const;
        void DrawOverlay(const EditorExtensionContext &context) const;
        void FlushHistory(History &history, World &world, ISceneOps &scene) const;
        bool OwnsViewport() const;
        std::vector<const char *> WindowNames() const;

    private:
        struct Entry
        {
            uint64_t mId = 0;
            IEditorExtension *mExtension = nullptr;
        };

        void Remove(uint64_t id);

        // Registration checks it; extension tokens must die before this registry
        std::shared_ptr<int> mOwner = std::make_shared<int>(0);
        std::vector<Entry> mEntries;
        uint64_t mNextId = 1;
    };
} // namespace mir
