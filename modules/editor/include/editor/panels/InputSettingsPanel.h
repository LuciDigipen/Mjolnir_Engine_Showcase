/**
 * @file InputSettingsPanel.h
 * @author Rahul Nair
 * @brief The "Input Settings" window - add/remove actions, add/remove/rebind
 *        bindings.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#pragma once

#include <input/InputMap.h>
#include <window/InputSnapshot.h>

#include <filesystem>
#include <string>

namespace mir
{
    class InputSettingsPanel
    {
    public:
        void Draw(InputMap &inputMap, const RawInputSnapshot &raw, const std::filesystem::path &savePath);

        void CancelListening() { mListeningAction = mListeningBinding = -1; }

        void OnHidden()
        {
            CancelListening();
            mStatusMessage.clear();
        }

    private:
        bool DrawAction(InputAction &action, int actionIndex, const RawInputSnapshot &raw);

        bool DrawBinding(const InputAction &action, InputBinding &binding, int actionIndex, int bindingIndex,
            const RawInputSnapshot &raw);

        bool TryCaptureBinding(InputBinding &binding, const RawInputSnapshot &raw);

        char mNewActionName[64] = {};
        int mNewActionType = 0;

        int mListeningAction = -1;
        int mListeningBinding = -1;
        RawInputSnapshot mPreviousRaw;

        std::string mStatusMessage;
        // The map as of the last Save; a further edit clears the message before it goes stale.
        InputMap mSavedCopy;
    };
}
