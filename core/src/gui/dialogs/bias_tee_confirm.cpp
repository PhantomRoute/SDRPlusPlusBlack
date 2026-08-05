#include <gui/dialogs/bias_tee_confirm.h>
#include <gui/dialogs/dialog_box.h>
#include <gui/smgui.h>
#include <core.h>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace dialogs {
    namespace {
        struct Pending {
            bool open = false;
            bool dontShowAgain = false;
        };

        // Keyed by widget label, so two radios each with their own Bias-T control
        // keep separate pending state.
        std::unordered_map<std::string, Pending> pendingByWidget;

        bool warningLoaded = false;
        bool warningEnabled = true;

        bool shouldWarn() {
            if (!warningLoaded) {
                core::configManager.acquire();
                if (core::configManager.conf.contains("biasTeeWarning")) {
                    warningEnabled = core::configManager.conf["biasTeeWarning"];
                }
                core::configManager.release();
                warningLoaded = true;
            }
            return warningEnabled;
        }

        void rememberChoice(bool warn) {
            warningEnabled = warn;
            warningLoaded = true;
            core::configManager.acquire();
            core::configManager.conf["biasTeeWarning"] = warn;
            core::configManager.release(true);
        }
    }

    bool BiasTeeCheckbox(const char* label, bool* value) {
        bool changed = SmGui::Checkbox(label, value);

        // In server mode the menu is drawn on the remote client, so there is no
        // window here to put a popup in and nothing on this end could dismiss it.
        // Let the change through rather than making Bias-T impossible to switch on.
        if (SmGui::isServerMode()) { return changed; }

        Pending& pending = pendingByWidget[label];

        // Only switching it on can do damage, and only while the user still wants
        // asking. Hold the value off until they accept, so the hardware is never
        // touched on the strength of a misclick.
        if (changed && *value && shouldWarn()) {
            *value = false;
            pending.open = true;
            pending.dontShowAgain = false;
            changed = false;
        }

        if (pending.open) {
            std::string id = std::string("bias_tee_confirm_") + label;
            int result = ImGui::GenericDialog(id.c_str(), pending.open, "Continue\0Stop\0", [&pending]() {
                ImGui::TextUnformatted("Enable Bias-T?");
                ImGui::Separator();
                ImGui::TextUnformatted("This feeds DC voltage out of the antenna connector.");
                ImGui::Spacing();
                ImGui::TextUnformatted("Check what is on the end of the coax first. A powered");
                ImGui::TextUnformatted("LNA or active antenna expects it. A plain antenna, a");
                ImGui::TextUnformatted("splitter with no DC block, or anything shorting the");
                ImGui::TextUnformatted("centre pin to the shield can damage the radio, the");
                ImGui::TextUnformatted("device on the end, or both.");
                ImGui::Spacing();
                ImGui::Checkbox("Don't show this again", &pending.dontShowAgain);
            });

            if (result >= 0) {
                if (pending.dontShowAgain) { rememberChoice(false); }
                // 0 is Continue, 1 is Stop.
                *value = (result == 0);
                changed = (result == 0);
            }
        }

        return changed;
    }
}
