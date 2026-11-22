/**
 * @file UIContext.h
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, the
 *        widgets' hot and active state, the draw list that is submitted
 *        when the frame ends, and the textures and fonts it draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include "UIBackend.h"
#include "UIDrawList.h"
#include "UIFonts.h"
#include "UIId.h"
#include "UILayout.h"
#include "UINavInput.h"
#include "UIState.h"
#include "UITextures.h"
#include "UITypes.h"

#include <input/InputMap.h>
#include <input/InputState.h>

#include <cstdint>
#include <optional>
#include <string_view>

namespace mir
{
    //what a button prompt shows for an action on the device the player last used
    struct UIPrompt
    {
        //the binding's short name, e.g. "A" or "Enter"; empty when the action
        //has no binding on that device
        std::string_view mLabel;

        //a gamepad face button, not a keyboard or mouse label
        bool mGamepad = false;
    };

    class UIContext
    {
    public:
        UIContext() = default;

        UIContext(const UIContext &) = delete;
        UIContext &operator=(const UIContext &) = delete;
        UIContext(UIContext &&) = delete;
        UIContext &operator=(UIContext &&) = delete;

        void BeginFrame(const UIRect &area, InputState &input, const InputMap &map, const UIPointer &pointer);

        void EndFrame();

        UIInteraction Interact(UIId id, const UIRect &rect, const UIWidgetOptions &options = {});

        UIDrawList &DrawList() { return mDrawList; }

        UIState &State() { return mState; }
        const UIState &State() const { return mState; }

        // the pointer in the coordinates of whatever is drawn now, under the current transform
        std::optional<UIPoint> LocalPointer() const { return mState.LocalPointer(mDrawList.CurrentTransform()); }


        const UIRect &Area() const { return mArea; }

        //area pixels per UI unit. An area smaller than the design size draws the
        //whole UI smaller to fit, text and all; a larger one never scales it up
        float Scale() const { return mScale; }

        //0 leaves that axis unscaled; from the next BeginFrame
        void SetDesignSize(float width, float height)
        {
            mDesignWidth = width;
            mDesignHeight = height;
        }

        //an area taller than this draws the UI larger in proportion, so it takes the same share of any display;
        //0 never scales it up. From the next BeginFrame
        void SetReferenceHeight(float height) { mReferenceHeight = height; }
        float ReferenceHeight() const { return mReferenceHeight; }

        //the area in UI units, which screens lay out, draw and hit-test in
        UIRect Bounds() const { return UIRect{0.0f, 0.0f, mArea.width / mScale, mArea.height / mScale}; }

        UIRect SafeArea() const { return Inset(Bounds(), mSafeAreaInsets); }
        void SetSafeAreaInsets(const UIEdges &insets) { mSafeAreaInsets = insets; }
        const UIEdges &SafeAreaInsets() const { return mSafeAreaInsets; }

        InputState *Input() const { return mInput; }

        const UINavInput &Nav() const { return mNav; }


        bool CancelPressed() const { return mNav.mCancel; }


        bool PausePressed() const { return mNav.mPause; }

        void HoldInput();

        bool HoldsInput() const { return mHoldsInput; }

        // HoldInput calls this frame, so a caller can tell whether the screens it ran held the input
        uint32_t HoldInputCalls() const { return mHoldInputCalls; }

        // Until it is turned off, every widget is disabled, drawn but not pressed, held or focused, and SetFocus is
        // refused: what a Game screen gets while a Shell screen holds the input
        void SetWidgetsDisabled(bool disabled) { mWidgetsDisabled = disabled; }
        bool WidgetsDisabled() const { return mWidgetsDisabled; }

        // UIState::SetFocus, unless widgets are disabled, false when refused
        bool SetFocus(UIId id);

        //the mouse cursor is hidden from the frame this is called in until a frame does not call it; the backend
        //bridge applies it
        void HideCursor();

        bool HidesCursor() const { return mHidesCursor; }

        bool FocusVisible() const { return mState.FocusVisible(); }

        //only between BeginFrame and EndFrame
        UIPrompt Prompt(std::string_view action) const;

        //in UI units in the style's font and size, measured by the backend at this frame's scale; an estimate
        //without a backend
        UISize MeasureText(std::string_view text) const;
        // in @p font at @p fontSize UI units, 0 the backend's own size
        UISize MeasureText(std::string_view text, UIFont font, float fontSize) const;
        float TextWidth(std::string_view text) const { return MeasureText(text).width; }

        UIStyle &Style() { return mStyle; }
        const UIStyle &Style() const { return mStyle; }

        UITextures &Textures() { return mTextures; }
        const UITextures &Textures() const { return mTextures; }

        UIFonts &Fonts() { return mFonts; }
        const UIFonts &Fonts() const { return mFonts; }

        // what EndFrame submits to; without one a frame runs and draws nothing.
        // Null before the backend goes; the textures and fonts it made stay
        void SetBackend(IUIBackend *backend)
        {
            mBackend = backend;
            mTextures.SetBackend(backend);
            mFonts.SetBackend(backend);
        }

    private:
        UITextures mTextures;
        UIFonts mFonts;
        IUIBackend *mBackend = nullptr;
        UIDrawList mDrawList;
        UIState mState;
        UIRect mArea;
        float mScale = 1.0f;
        //every screen the engine ships fits 640 x 480 at its own size
        float mDesignWidth = 640.0f;
        float mDesignHeight = 480.0f;
        float mReferenceHeight = 720.0f;
        UIEdges mSafeAreaInsets;
        InputState *mInput = nullptr;
        const InputMap *mInputMap = nullptr;
        UINavInput mNav;
        UIStyle mStyle;
        bool mInFrame = false;
        bool mHoldInput = false;
        bool mHoldsInput = false;
        uint32_t mHoldInputCalls = 0;
        bool mWidgetsDisabled = false;
        bool mHideCursor = false;
        bool mHidesCursor = false;
    };
} // namespace mir
