#pragma once

namespace dialogs {
    // A checkbox for a setting that can destroy hardware if it is switched on by
    // mistake - Bias-T putting voltage on the antenna port, an RF amplifier that a
    // strong signal will burn out.
    //
    // Drop-in replacement for SmGui::Checkbox. Returns true when the value changed
    // and the caller should push it to the hardware, exactly as SmGui::Checkbox does.
    // The difference is that switching it *on* asks first, and only reports the
    // change once the user has accepted, so a misclick never reaches the radio.
    // Switching it off is never held up.
    //
    // Most warnings can be silenced from inside their own dialog. That choice goes in
    // the main config under the warning's configKey and applies to every source using
    // the same warning - silencing one kind does not silence the other. A warning that
    // sets allowSilence false cannot be turned off, which is for the ones where the
    // answer has to be given every time.

    // Accepting takes two separate deliberate actions rather than one button press,
    // because a single stray tap is the difference between a working radio and a dead
    // one:
    //
    //  - the dialog opens in the middle of the screen, not under the control that was
    //    touched, so the tap that opened it cannot carry through into it
    //  - a tick box has to be ticked before the accept button does anything, and the
    //    two are in different places
    //  - the accept button is the far one; the near one cancels
    //  - the tick box is cleared every time the dialog opens
    //
    // A fat finger can hit any one of those. It cannot hit all of them.
    struct HazardWarning {
        const char* configKey;    // Main config flag holding "do not ask me again"
        const char* question;     // Heading, e.g. "Enable Bias-T?"
        const char* const* body;  // Lines of explanation, NULL terminated
        bool allowSilence;        // Whether the dialog offers "Don't show this again"
        const char* armLabel;     // The tick box that arms the accept button
        const char* confirmLabel; // The accept button
    };

    bool HazardCheckbox(const char* label, bool* value, const HazardWarning& warning);

    // Voltage out of the antenna connector.
    bool BiasTeeCheckbox(const char* label, bool* value);

    // A fixed gain amplifier at the front of the receiver.
    bool RfAmpCheckbox(const char* label, bool* value);
}
