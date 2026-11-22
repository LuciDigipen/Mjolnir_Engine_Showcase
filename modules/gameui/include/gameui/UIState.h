/**
 * @file UIState.h
 * @author Quy Dong Ta
 * @brief Which widget is under the pointer (hot), which one it pressed
 *        (active, latched until release), which one has focus, the hit test,
 *        and the explicit links focus moves along.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIId.h"
#include "UINavInput.h"
#include "UITransform.h"
#include "UITypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace mir
{

    struct UIPointer
    {
        float mX = 0.0f;
        float mY = 0.0f;

        // The primary button
        bool mDown = false;

        // Something above the game UI has the pointer, such as an editor popup: nothing is hovered or pressed, and
        // a held widget lets go without a click
        bool mWithheld = false;
    };



    using UINavLinks = std::array<UIId, 4>;

    struct UIWidgetOptions
    {
        UINavLinks mLinks{};

        bool mHidden = false;

        bool mDisabled = false;

        //while focused, UINavigate left and right come to the widget as
        //UIInteraction::mAdjust instead of following its left and right links
        bool mAdjustsX = false;

        //clicked by the pointer, never focused: no link, hover, press or first
        //UINavigate lands on it, and links pass over it. For a HUD button in
        //play, where the stick and the confirm button belong to the game
        bool mPointerOnly = false;
    };

    struct UIInteraction
    {

        bool mHovered = false;

        bool mHeld = false;

        //pointer release over the widget, or UIConfirm while it has focus
        bool mClicked = false;

        //as of the last EndFrame
        bool mFocused = false;

        //mFocused, while the focus highlight is shown; draw the highlight from this
        bool mShowFocus = false;

        //-1 or +1 on the frame UINavigate steps left or right, for a focused,
        //enabled widget with mAdjustsX; otherwise 0
        int mAdjust = 0;
    };

    class UIState
    {
    public:

        //focusVisible: whether this frame draws the focus highlight. The frame it
        //comes back on, UINavigate and UIConfirm only bring it back, unless
        //nothing is focused, when UINavigate still lands on the first widget.
        //scale: area pixels per UI unit, as UIContext::Scale
        void BeginFrame(const UIRect &area, const UIPointer &pointer, const UINavInput &nav = {},
                        bool focusVisible = true, float scale = 1.0f);


        void EndFrame();

        // rect is where the widget is drawn, before transform, clip is in the area's units, as the draw list keeps it
        UIInteraction Interact(UIId id, const UIRect &rect, const UIRect &clip, const UIWidgetOptions &options = {},
                               const UITransform &transform = {});

        bool HitTest(const UIRect &rect, const UIRect &clip, const UITransform &transform = {}) const;

        UIId FollowLink(UIId from, UINavDirection direction) const;

        UIId Hot() const { return mHot; }
        UIId Active() const { return mActive; }


        UIId Focus() const { return mFocus; }

        void SetFocus(UIId id) { mFocus = id; }

        bool FocusVisible() const { return mFocusVisible; }

        bool PointerWithheld() const { return mPointerWithheld; }
        float PointerX() const { return mPointerX; }
        float PointerY() const { return mPointerY; }
        // the pointer back through transform, where a widget drawn under it would see it. Empty when the
        // transform flattens everything
        std::optional<UIPoint> LocalPointer(const UITransform &transform) const;
        bool PointerMoved() const { return mPointerMoved; }

        size_t Collisions() const { return mCollisions; }

    private:
        struct Declared
        {
            UINavLinks mLinks{};
            //enabled and not pointer-only: what focus may land on
            bool mFocusable = true;
        };

        bool IsFocusable(UIId id) const;

        //the area in UI units, at the origin, like the pointer
        UIRect mBounds;
        float mPointerX = 0.0f;
        float mPointerY = 0.0f;
        bool mDown = false;
        bool mPointerWithheld = false;

        bool mPressed = false;

        //window pixels, not local, so a dock drag under a still pointer is not a move
        float mLastPointerX = 0.0f;
        float mLastPointerY = 0.0f;
        bool mHasLastPointer = false;
        bool mPointerMoved = false;

        UINavStepper mNavStepper;
        std::optional<UINavDirection> mNavDirection;
        bool mConfirm = false;

        //a widget took this frame's step as mAdjust, so focus does not follow it
        bool mNavAdjusted = false;

        bool mFocusVisible = true;
        //the highlight comes back this frame
        bool mRevealFocus = false;

        UIId mHot;
        UIId mActive;
        UIId mFocus;

        UIId mNextHot;

        bool mActiveSeen = false;

        //this frame's widgets by id, the first declaration of a colliding id wins
        std::unordered_map<uint64_t, Declared> mDeclared;
        UIId mFirstFocusable;
        size_t mCollisions = 0;

        std::unordered_set<uint64_t> mLoggedCollisions;
    };
} // namespace mir
