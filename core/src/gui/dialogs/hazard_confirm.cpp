#include <gui/dialogs/hazard_confirm.h>
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

        // Keyed by widget label, so two radios each with their own control keep
        // separate pending state.
        std::unordered_map<std::string, Pending> pendingByWidget;

        // Keyed by config key, so silencing one kind of warning leaves the others
        // alone. Cached because this is read on every frame the menu is drawn.
        std::unordered_map<std::string, bool> warningEnabledByKey;

        bool shouldWarn(const char* configKey) {
            auto it = warningEnabledByKey.find(configKey);
            if (it != warningEnabledByKey.end()) { return it->second; }

            bool enabled = true;
            core::configManager.acquire();
            if (core::configManager.conf.contains(configKey) &&
                core::configManager.conf[configKey].is_boolean()) {
                enabled = core::configManager.conf[configKey];
            }
            core::configManager.release();
            warningEnabledByKey[configKey] = enabled;
            return enabled;
        }

        void rememberChoice(const char* configKey, bool warn) {
            warningEnabledByKey[configKey] = warn;
            core::configManager.acquire();
            core::configManager.conf[configKey] = warn;
            core::configManager.release(true);
        }
    }

    bool HazardCheckbox(const char* label, bool* value, const HazardWarning& warning) {
        bool changed = SmGui::Checkbox(label, value);

        // In server mode the menu is drawn on the remote client, so there is no
        // window here to put a popup in and nothing on this end could dismiss it.
        // Let the change through rather than making the setting impossible to reach.
        if (SmGui::isServerMode()) { return changed; }

        Pending& pending = pendingByWidget[label];

        // Only switching it on can do damage, and only while the user still wants
        // asking. Hold the value off until they accept, so the hardware is never
        // touched on the strength of a misclick.
        // A warning that cannot be silenced is not read from the config either, or a
        // hand written flag - or a config copied from another machine - would turn it
        // off through the back door.
        bool warn = !warning.allowSilence || shouldWarn(warning.configKey);
        if (changed && *value && warn) {
            *value = false;
            pending.open = true;
            pending.dontShowAgain = false;
            changed = false;
        }

        if (pending.open) {
            std::string id = std::string(warning.configKey) + "_confirm_" + label;
            int result = ImGui::GenericDialog(id.c_str(), pending.open, "Continue\0Stop\0", [&pending, &warning]() {
                ImGui::TextUnformatted(warning.question);
                ImGui::Separator();
                for (const char* const* line = warning.body; *line != NULL; line++) {
                    if (**line == '\0') {
                        ImGui::Spacing();
                        continue;
                    }
                    ImGui::TextUnformatted(*line);
                }
                if (warning.allowSilence) {
                    ImGui::Spacing();
                    ImGui::Checkbox("Don't show this again", &pending.dontShowAgain);
                }
            });

            if (result >= 0) {
                if (pending.dontShowAgain) { rememberChoice(warning.configKey, false); }
                // 0 is Continue, 1 is Stop.
                *value = (result == 0);
                changed = (result == 0);
            }
        }

        return changed;
    }

    namespace {
        const char* const BIAS_TEE_BODY[] = {
            "This feeds DC voltage out of the antenna connector.",
            "",
            "Check what is on the end of the coax first. A powered",
            "LNA or active antenna expects it. A plain antenna, a",
            "splitter with no DC block, or anything shorting the",
            "centre pin to the shield can damage the radio, the",
            "device on the end, or both.",
            NULL
        };

        // The HackRF's is the one people lose. It is a fixed gain stage right at the
        // antenna port with nothing in front of it, and it amplifies the whole band
        // rather than the part on screen - so a broadcast transmitter three hundred
        // megahertz away from where you are tuned still goes through it.
        const char* const RF_AMP_BODY[] = {
            "This switches a fixed amplifier into the antenna path,",
            "ahead of everything else in the radio.",
            "",
            "It amplifies the whole band, not just what is on screen,",
            "so a strong signal anywhere in range - broadcast FM, TV,",
            "pagers, a transmitter in the same room - can destroy it",
            "while you are tuned somewhere else entirely.",
            "",
            "Turn the LNA and VGA gain up first. Those are safe, and",
            "usually enough. Leave this off unless the band is quiet",
            "or there is a filter in front of it.",
            NULL
        };

        const HazardWarning BIAS_TEE_WARNING = {
            "biasTeeWarning",
            "Enable Bias-T?",
            BIAS_TEE_BODY,
            true
        };

        // No "don't show this again" on this one. It is the only setting here that
        // destroys the radio itself rather than something on the end of the coax, and
        // it is off at the start of every session, so being asked is at worst once per
        // session. Silencing it would leave a single misclick between a busy band and
        // a dead front end. Set this to true to allow it to be turned off.
        const HazardWarning RF_AMP_WARNING = {
            "rfAmpWarning",
            "Enable the RF amplifier?",
            RF_AMP_BODY,
            false
        };
    }

    bool BiasTeeCheckbox(const char* label, bool* value) {
        return HazardCheckbox(label, value, BIAS_TEE_WARNING);
    }

    bool RfAmpCheckbox(const char* label, bool* value) {
        return HazardCheckbox(label, value, RF_AMP_WARNING);
    }
}
