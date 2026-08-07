#pragma once
#include <gui/widgets/waterfall.h>
#include <gui/widgets/frequency_select.h>
#include <gui/widgets/menu.h>
#include <gui/dialogs/loading_screen.h>
#include <module.h>
#include <gui/brown/mobile_main_window.h>
#include <gui/theme_manager.h>

namespace gui {
    SDRPP_EXPORT ImGui::WaterFall waterfall;
    SDRPP_EXPORT FrequencySelect freqSelect;
    SDRPP_EXPORT Menu menu;
    SDRPP_EXPORT ThemeManager themeManager;
    SDRPP_EXPORT MobileMainWindow& mainWindow;

    struct VFOFrequencyChange {
//        WaterfallVFO* vfo;
        double freq;
    };

    SDRPP_EXPORT Event<VFOFrequencyChange> vfoFrequencyChanged;

    // True while an ImGui text field has the keyboard. The bare key shortcuts -
    // space to record a voice command, End to start and stop, the arrows and page
    // keys to tune - all read the raw key state, which ImGui does not suppress for
    // us, so typing a theme name or a bookmark name would otherwise fire them.
    inline bool imguiWantsKeyboard() { return ImGui::GetIO().WantCaptureKeyboard; }

    void selectSource(std::string name);
};