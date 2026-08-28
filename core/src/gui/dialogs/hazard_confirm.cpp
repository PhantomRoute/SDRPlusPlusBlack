#include <gui/dialogs/hazard_confirm.h>
#include <gui/dialogs/dialog_box.h>
#include <gui/smgui.h>
#include <gui/style.h>
#include <core.h>
#include <imgui.h>
#include <string>
#include <unordered_map>

namespace dialogs {
    namespace {
        struct Pending {
            bool open = false;
            bool dontShowAgain = false;
            bool armed = false;
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

        void drawBody(const HazardWarning& warning) {
            ImGui::TextUnformatted(warning.question);
            ImGui::Separator();
            for (const char* const* line = warning.body; *line != NULL; line++) {
                if (**line == '\0') {
                    ImGui::Spacing();
                    continue;
                }
                ImGui::TextUnformatted(*line);
            }
        }
    }

    bool HazardCheckbox(const char* label, bool* value, const HazardWarning& warning) {
        bool changed = SmGui::Checkbox(label, value);

        // In server mode the menu is drawn on the remote client, so there is no
        // window here to put a popup in and nothing on this end could dismiss it.
        // Let the change through rather than making the setting impossible to reach.
        if (SmGui::isServerMode()) { return changed; }

        Pending& pending = pendingByWidget[label];

        // A warning that cannot be silenced is not read from the config either, or a
        // hand written flag - or a config copied from another machine - would turn it
        // off through the back door.
        bool warn = !warning.allowSilence || shouldWarn(warning.configKey);

        // Only switching it on can do damage, and only while the user still wants
        // asking. Hold the value off until they accept, so the hardware is never
        // touched on the strength of a misclick.
        if (changed && *value && warn) {
            *value = false;
            pending.open = true;
            pending.dontShowAgain = false;
            pending.armed = false;
            changed = false;
        }

        if (!pending.open) { return changed; }

        std::string id = std::string(warning.configKey) + "_confirm_" + label;

        // See the comment on HazardWarning for what each part of this is defending
        // against.
        int decision = -1;

        // Away from whatever was touched to get here. A popup opens where the cursor
        // is, which on a touchscreen is directly under the finger that opened it - so
        // the second half of a double tap, or a finger held a moment too long, lands
        // inside a dialog that was not on screen when the gesture started.
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x * 0.5f), vp->Pos.y + (vp->Size.y * 0.5f)),
                                ImGuiCond_Always, ImVec2(0.5f, 0.5f));

        ImGui::GenericDialog(id.c_str(), pending.open, "", [&pending, &warning, &decision]() {
            drawBody(warning);

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Checkbox(warning.armLabel, &pending.armed);

            ImGui::Spacing();
            ImGui::Spacing();

            // Cancel near, accept far, with a gap between them rather than the usual
            // pairing, so the two are not one thumb width apart. The accept does
            // nothing at all until the box above has been ticked, which is what stops
            // a tap arriving before the dialog has been read.
            const ImVec2 size(120.0f * style::uiScale, 0.0f);
            if (ImGui::Button("Cancel", size)) { decision = 0; }
            ImGui::SameLine(0.0f, 60.0f * style::uiScale);
            if (!pending.armed) { ImGui::BeginDisabled(); }
            if (ImGui::Button(warning.confirmLabel, size)) { decision = 1; }
            if (!pending.armed) { ImGui::EndDisabled(); }

            // Below the buttons, behind a rule of its own. It is the one control here
            // that has effect beyond this dialog, and putting it next to the tick box
            // that arms the accept would make the two easy to confuse - tick the wrong
            // one and the warning is gone for good rather than armed for a moment.
            if (warning.allowSilence) {
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
                ImGui::Checkbox("Don't show this again", &pending.dontShowAgain);
            }
        });

        if (decision >= 0) {
            if (pending.dontShowAgain) { rememberChoice(warning.configKey, false); }
            pending.open = false;
            *value = (decision == 1);
            changed = (decision == 1);
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

        // Two steps as well. Feeding DC into something that cannot take it kills
        // whatever is on the end of the coax, and a phone screen makes no distinction
        // between a tap meant for this and a tap meant for the row above it.
        //
        // This one can still be silenced, unlike the amp: it is switched on
        // deliberately every session by anyone running a powered LNA, and it is
        // remembered per device, so being asked forever is a real cost. Silencing it
        // does drop the two steps with it - that is what silencing means.
        const HazardWarning BIAS_TEE_WARNING = {
            "biasTeeWarning",
            "Enable Bias-T?",
            BIAS_TEE_BODY,
            true,
            "What is on the coax expects DC power",
            "Enable Bias-T"
        };

        // No "don't show this again" on this one, and two separate actions to accept.
        // It is the only setting here that destroys the radio itself rather than
        // something on the end of the coax, and it is off at the start of every
        // session, so being asked is at worst once per session. One stray tap should
        // not be the difference between a working front end and a dead one.
        const HazardWarning RF_AMP_WARNING = {
            "rfAmpWarning",
            "Enable the RF amplifier?",
            RF_AMP_BODY,
            false,
            "I have checked what is connected to the antenna",
            "Enable amp"
        };
    }

    bool BiasTeeCheckbox(const char* label, bool* value) {
        return HazardCheckbox(label, value, BIAS_TEE_WARNING);
    }

    bool RfAmpCheckbox(const char* label, bool* value) {
        return HazardCheckbox(label, value, RF_AMP_WARNING);
    }
}
