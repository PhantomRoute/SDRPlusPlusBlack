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

    struct HazardWarning {
        const char* configKey;    // Main config flag holding "do not ask me again"
        const char* question;     // Heading, e.g. "Enable Bias-T?"
        const char* const* body;  // Lines of explanation, NULL terminated
        bool allowSilence;        // Whether the dialog offers "Don't show this again"
    };

    bool HazardCheckbox(const char* label, bool* value, const HazardWarning& warning);

    // Voltage out of the antenna connector.
    bool BiasTeeCheckbox(const char* label, bool* value);

    // A fixed gain amplifier at the front of the receiver.
    bool RfAmpCheckbox(const char* label, bool* value);
}
