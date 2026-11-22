/**
 * @file UIContext.cpp
 * @author Quy Dong Ta
 * @brief The game UI's frame: the area it draws into, the input it reads, the
 *        widgets' hot and active state, the draw list that is submitted
 *        when the frame ends, and the textures and fonts it draws.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <gameui/UIContext.h>

#include <core/log/Assert.h>
#include <input/PromptLabel.h>

#include <algorithm>
#include <limits>

namespace mir
{
    namespace
    {
        // Below the design size, shrunk to fit it. Above the reference height, grown with the height, but never so
        // far that the design size stops fitting
        float FitScale(const UIRect &area, float designWidth, float designHeight, float referenceHeight)
        {
            float fit = std::numeric_limits<float>::max();
            if (designWidth > 0.0f && area.width > 0.0f)
                fit = std::min(fit, area.width / designWidth);
            if (designHeight > 0.0f && area.height > 0.0f)
                fit = std::min(fit, area.height / designHeight);
            if (fit < 1.0f)
                return fit;

            const float grown = referenceHeight > 0.0f ? area.height / referenceHeight : 1.0f;
            return std::min(std::max(grown, 1.0f), fit);
        }
    } // namespace

    void UIContext::BeginFrame(const UIRect &area, InputState &input, const InputMap &map, const UIPointer &pointer)
    {
        MIR_ASSERT(!mInFrame, "UIContext::BeginFrame twice without EndFrame");

        mArea = area;
        mScale = FitScale(area, mDesignWidth, mDesignHeight, mReferenceHeight);
        mInput = &input;
        mInputMap = &map;
        mNav = ReadNavInput(input);
        const UIRect bounds = Bounds();
        mDrawList.Reset(bounds.width, bounds.height);

        const bool focusVisible = input.ActiveDevice() != DeviceKind::Mouse;
        mState.BeginFrame(area, pointer, mNav, focusVisible, mScale);
        mHoldInputCalls = 0;
        mWidgetsDisabled = false;
        mInFrame = true;
    }

    void UIContext::EndFrame()
    {
        MIR_ASSERT(mInFrame, "UIContext::EndFrame without BeginFrame");
        MIR_ASSERT(mDrawList.ClipDepth() == 0, "UIContext::EndFrame with {} PushClip unpopped",
                   mDrawList.ClipDepth());
        MIR_ASSERT(mDrawList.OpacityDepth() == 0, "UIContext::EndFrame with {} PushOpacity unpopped",
                   mDrawList.OpacityDepth());

        if (mInFrame && !mArea.IsEmpty() && mBackend != nullptr)
            mBackend->Submit(mArea, mScale, mDrawList, mTextures, mFonts);

        if (mInFrame)
        {
            mState.EndFrame();


            mHoldsInput = mHoldInput;
            mInput->SetGameInputHeld(mHoldsInput);
            mHidesCursor = mHideCursor;
        }
        mHoldInput = false;
        mHideCursor = false;

        mInput = nullptr;
        mInputMap = nullptr;
        mNav = UINavInput{};
        mInFrame = false;
    }

    void UIContext::HoldInput()
    {
        MIR_ASSERT(mInFrame, "UIContext::HoldInput outside BeginFrame/EndFrame");
        mHoldInput = mInFrame;
        ++mHoldInputCalls;
    }

    void UIContext::HideCursor()
    {
        MIR_ASSERT(mInFrame, "UIContext::HideCursor outside BeginFrame/EndFrame");
        mHideCursor = mInFrame;
    }

    UIInteraction UIContext::Interact(UIId id, const UIRect &rect, const UIWidgetOptions &options)
    {
        MIR_ASSERT(mInFrame, "UIContext::Interact outside BeginFrame/EndFrame");

        if (!mWidgetsDisabled)
            return mState.Interact(id, rect, mDrawList.CurrentClip(), options);
        UIWidgetOptions disabled = options;
        disabled.mDisabled = true;
        return mState.Interact(id, rect, mDrawList.CurrentClip(), disabled);
    }

    bool UIContext::SetFocus(UIId id)
    {
        if (mWidgetsDisabled)
            return false;
        mState.SetFocus(id);
        return true;
    }

    UIPrompt UIContext::Prompt(std::string_view action) const
    {
        MIR_ASSERT(mInFrame, "UIContext::Prompt outside BeginFrame/EndFrame");
        if (!mInFrame)
            return {};

        const DeviceKind device = mInput->ActiveDevice();
        return UIPrompt{PromptLabel(*mInputMap, action, device), device == DeviceKind::Gamepad};
    }

    UISize UIContext::MeasureText(std::string_view text) const
    {
        return MeasureText(text, mStyle.mFont, mStyle.mFontSize);
    }

    UISize UIContext::MeasureText(std::string_view text, UIFont font, float fontSize) const
    {
        if (mBackend == nullptr)
            return EstimateTextSize(text, fontSize > 0.0f ? fontSize : mStyle.mTextHeight);

        void *backendFont = mFonts.Find(font);

        // the backend's own size is not scaled when drawn, so it is already in UI units
        if (fontSize <= 0.0f)
            return mBackend->MeasureText(text, 0.0f, backendFont);

        // measured at the size it is drawn, since a font's widths do not scale exactly
        const UISize pixels = mBackend->MeasureText(text, fontSize * mScale, backendFont);
        return UISize{pixels.width / mScale, pixels.height / mScale};
    }
} // namespace mir
