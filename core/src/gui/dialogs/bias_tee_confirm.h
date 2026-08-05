#pragma once

namespace dialogs {
    // Drop-in replacement for SmGui::Checkbox on a Bias-T control.
    //
    // Returns true when the value changed and the caller should push it to the
    // hardware, exactly as SmGui::Checkbox does. The difference is that switching
    // Bias-T *on* asks for confirmation first, and only reports the change once the
    // user has accepted it - so a misclick cannot put voltage on the antenna port.
    // Switching it off is never held up.
    //
    // The user can silence the warning from inside the dialog; that choice is
    // stored in the main config as "biasTeeWarning" and applies to every source.
    bool BiasTeeCheckbox(const char* label, bool* value);
}
