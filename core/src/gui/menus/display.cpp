
#include <gui/menus/display.h>
#include <imgui.h>
#include <gui/gui.h>
#include <core.h>
#include <gui/colormaps.h>
#include <gui/gui.h>
#include <gui/main_window.h>
#include <signal_path/signal_path.h>
#include <gui/style.h>
#include <utils/optionlist.h>
#include <algorithm>

namespace displaymenu {
    bool showWaterfall;
    bool showFFT = true;
    bool showMicHistogram = false;
    bool fullWaterfallUpdate = true;
    bool showBattery = true;
    bool detectSignals = false;

    // Handler for center frequency changes
    EventHandler<double> centerFreqChangedHandler;
    std::string currentBatteryLevel = "?";
    int colorMapId = 0;
    std::vector<std::string> colorMapNames;
    std::string colorMapNamesTxt = "";
    std::string colorMapAuthor = "";
    int selectedWindow = 0;
    int fftRate = 20;
    int uiScaleId = 0;
    int transcieverLayoutId = 0;
    bool restartRequired = false;
    bool fftHold = false;
    int fftHoldSpeed = 60;
    bool fftSmoothing = false;
    int fftSmoothingSpeed = 100;
    bool phoneLayout = false;

    Event<ImGuiContext *> onDisplayDraw;

    TranscieverLayout transcieverLayout = TRAL_NONE;

#ifdef __ANDROID__
    float displayDensity = 1.0;  // 1.0 = 160 dpi. 3.5 = kinda high dpi etc. Coincides with good default scale
#endif
    bool snrSmoothing = false;
    int snrSmoothingSpeed = 20;

    OptionList<float, float> uiScales;

    const int FFTSizes[] = {
        524288,
        262144,
        131072,
        65536,
        32768,
        16384,
        8192,
        4096,
        2048,
        1024
    };

    const char* FFTSizesStr = "524288\0"
                              "262144\0"
                              "131072\0"
                              "65536\0"
                              "32768\0"
                              "16384\0"
                              "8192\0"
                              "4096\0"
                              "2048\0"
                              "1024\0"
#ifdef __APPLE__
                            "524288 ACCEL\0"
                            "262144 ACCEL\0"
                            "131072 ACCEL\0"
                            "65536 ACCEL\0"
                            "32768 ACCEL\0"
                            "16384 ACCEL\0"
                            "8192 ACCEL\0"
                            "4096 ACCEL\0"
                            "2048 ACCEL\0"
                            "1024 ACCEL\0"
#endif
        ;

    int fftSizeId = 0;

    const IQFrontEnd::FFTWindow fftWindowList[] = {
        IQFrontEnd::FFTWindow::RECTANGULAR,
        IQFrontEnd::FFTWindow::BLACKMAN,
        IQFrontEnd::FFTWindow::NUTTALL,
    };

    void updateFFTSpeeds() {
        gui::waterfall.setFFTHoldSpeed((float)fftHoldSpeed / ((float)fftRate * 10.0f));
        gui::waterfall.setFFTSmoothingSpeed(std::min<float>((float)fftSmoothingSpeed / (float)(fftRate * 10.0f), 1.0f));
        gui::waterfall.setSNRSmoothingSpeed(std::min<float>((float)snrSmoothingSpeed / (float)(fftRate * 10.0f), 1.0f));
    }

    void init() {
        if (core::configManager.conf.contains("showFFT")) {
            showFFT = core::configManager.conf["showFFT"];
        }
        if (core::configManager.conf.contains("showMicHistogram")) {
            showMicHistogram = core::configManager.conf["showMicHistogram"];
        }
        showWaterfall = core::configManager.conf["showWaterfall"];
        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        std::string colormapName = core::configManager.conf["colorMap"];
        if (colormaps::maps.find(colormapName) != colormaps::maps.end()) {
            colormaps::Map map = colormaps::maps[colormapName];
            gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
        }

        for (auto const& [name, map] : colormaps::maps) {
            colorMapNames.push_back(name);
            colorMapNamesTxt += name;
            colorMapNamesTxt += '\0';
            if (name == colormapName) {
                colorMapId = (colorMapNames.size() - 1);
                colorMapAuthor = map.author;
            }
        }

        fullWaterfallUpdate = core::configManager.conf["fullWaterfallUpdate"];
        gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);

        if (core::configManager.conf.contains("showBattery")) {
            showBattery = core::configManager.conf["showBattery"];
        }
        if (core::configManager.conf.contains("detectSignals")) {
            detectSignals = core::configManager.conf["detectSignals"];
            sigpath::iqFrontEnd.togglePreprocessor(&sigpath::iqFrontEnd.detectorPreprocessor, detectSignals);
        }

        // Connect to center frequency change event
        sigpath::sourceManager.onTuneChanged.bindHandler(&centerFreqChangedHandler);
        centerFreqChangedHandler.ctx = NULL;
        centerFreqChangedHandler.handler = [](double freq, void* ctx) {
            sigpath::iqFrontEnd.detectorPreprocessor.setCenterFrequency(freq);
        };

        fftSizeId = 4;
        int fftSize = core::configManager.conf["fftSize"];
        for (int i = 0; i < std::size(FFTSizes); i++) {
            if (fftSize == FFTSizes[i]) {
                fftSizeId = i;
                break;
            }
        }
        auto accel = core::configManager.conf.contains("fftAccel") && core::configManager.conf["fftAccel"];
        if (accel) {
            fftSizeId += std::size(FFTSizes);
        }

        enableAcceleratedFFT = accel;
        sigpath::iqFrontEnd.setFFTSize(FFTSizes[fftSizeId % std::size(FFTSizes)]);

        fftRate = core::configManager.conf["fftRate"];
        sigpath::iqFrontEnd.setFFTRate(fftRate);

        selectedWindow = std::clamp<int>((int)core::configManager.conf["fftWindow"], 0, (sizeof(fftWindowList) / sizeof(IQFrontEnd::FFTWindow)) - 1);
        sigpath::iqFrontEnd.setFFTWindow(fftWindowList[selectedWindow]);

        gui::menu.locked = core::configManager.conf["lockMenuOrder"];

        phoneLayout = core::configManager.conf["smallScreen"];
        transcieverLayout = core::configManager.conf["transcieverLayout"];

        fftHold = core::configManager.conf["fftHold"];
        fftHoldSpeed = core::configManager.conf["fftHoldSpeed"];
        gui::waterfall.setFFTHold(fftHold);
        fftSmoothing = core::configManager.conf["fftSmoothing"];
        fftSmoothingSpeed = core::configManager.conf["fftSmoothingSpeed"];
        gui::waterfall.setFFTSmoothing(fftSmoothing);
        snrSmoothing = core::configManager.conf["snrSmoothing"];
        snrSmoothingSpeed = core::configManager.conf["snrSmoothingSpeed"];
        gui::waterfall.setSNRSmoothing(snrSmoothing);
        updateFFTSpeeds();


        // Define and load UI scales

        std::vector<float> scales = {0.25f, 0.5f, 0.66f, 0.75f, 0.9f, 1.0f, 1.25f, 1.5f, 1.75f, 2.0f, 2.5f, 3.0f, 4.0f};
        bool hasNativeScale = false;
#ifdef __ANDROID__
        for (int i = 0; i < scales.size(); i++) {
            float scale = scales[i];
            if (scale == displayDensity) {
                hasNativeScale = true;
            }
        }
        if (!hasNativeScale) {
            uiScales.define(displayDensity, std::to_string((int)(displayDensity * 100)) + "% (native)", displayDensity);
        }
#endif
        for (int i = 0; i < scales.size(); i++) {
            float scale = scales[i];
            uiScales.define(scale, std::to_string((int)(scale * 100)) + "%", scale);
        }

        uiScaleId = uiScales.valueId(style::uiScale);
    }

    void setWaterfallShown(bool shown) {
        showWaterfall = shown;
        showWaterfall ? gui::waterfall.showWaterfall() : gui::waterfall.hideWaterfall();
        core::configManager.acquire();
        core::configManager.conf["showWaterfall"] = showWaterfall;
        core::configManager.release(true);
    }


    void checkKeybinds() {
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false) && !gui::imguiWantsKeyboard()) {
            setWaterfallShown(!showWaterfall);
        }
    }


    std::string getColorMapName() {
        if (colorMapId < 0 || colorMapId >= (int)colorMapNames.size()) { return ""; }
        return colorMapNames[colorMapId];
    }

    // Returns false for a gradient this install doesn't have, which is what a theme
    // from someone with extra colormaps installed will ask for. The rest of the theme
    // still applies; only the gradient is left alone.
    bool setColorMapByName(const std::string& name) {
        auto it = std::find(colorMapNames.begin(), colorMapNames.end(), name);
        if (it == colorMapNames.end() || colormaps::maps.find(name) == colormaps::maps.end()) {
            return false;
        }
        colorMapId = std::distance(colorMapNames.begin(), it);
        colormaps::Map map = colormaps::maps[name];
        gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
        colorMapAuthor = map.author;
        core::configManager.acquire();
        core::configManager.conf["colorMap"] = name;
        core::configManager.release(true);
        return true;
    }

    void drawColorMapSelector() {
        if (colorMapNames.empty()) { return; }
        float menuWidth = ImGui::GetContentRegionAvail().x;
        ImGui::LeftLabel("Color Map");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo("##_sdrpp_color_map_sel", &colorMapId, colorMapNamesTxt.c_str())) {
            colormaps::Map map = colormaps::maps[colorMapNames[colorMapId]];
            gui::waterfall.updatePalletteFromArray(map.map, map.entryCount);
            core::configManager.acquire();
            core::configManager.conf["colorMap"] = colorMapNames[colorMapId];
            core::configManager.release(true);
            colorMapAuthor = map.author;
        }
        ImGui::Text("Color map Author: %s", colorMapAuthor.c_str());
    }

    // A (?) that explains a control without spending a line on it.
    void helpMarker(const char* text) {
        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered()) { ImGui::SetTooltip("%s", text); }
    }

    // This menu is one long list of switches with nothing to say which of them
    // belong together. These break it into the three things it actually covers:
    // the window layout, what the spectrum looks like, and what it costs to run.
    void sectionHeader(const char* title) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", title);
        ImGui::Separator();
    }

    void draw(void* ctx) {
        sectionHeader("LAYOUT");

        if (ImGui::Checkbox("Waterfall##_sdrpp", &showWaterfall)) {
            setWaterfallShown(showWaterfall);
        }
        helpMarker("Show the scrolling history under the spectrum. The Home key toggles this too.");
        ImGui::SameLine();
        if (ImGui::Checkbox("FFT##_sdrpp", &showFFT)) {
            core::configManager.acquire();
            core::configManager.conf["showFFT"] = showFFT;
            core::configManager.release(true);
        }
        helpMarker("Show the live spectrum trace above the waterfall.");

        if (ImGui::Checkbox("Big controls (small screen, thick fingers)##_sdrpp", &phoneLayout)) {
            core::configManager.acquire();
            core::configManager.conf["smallScreen"] = phoneLayout;
            core::configManager.release(true);
        }
        helpMarker("Taller menu rows, fatter scrollbars and a shorter frequency readout,\nfor touch screens and small displays.");

        ImGui::LeftLabel("Layout");
        if (ImGui::RadioButton("Default ##_sdrpp", transcieverLayout == TRAL_NONE)) {
            core::configManager.acquire();
            transcieverLayout = TRAL_NONE;
            core::configManager.conf["transcieverLayout"] = transcieverLayout;
            core::configManager.release(true);
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("SSB trx ##_sdrpp", transcieverLayout == TRAL_SSB_FIRST)) {
            core::configManager.acquire();
            transcieverLayout = TRAL_SSB_FIRST;
            core::configManager.conf["transcieverLayout"] = transcieverLayout;
            core::configManager.release(true);
        }
        helpMarker("SSB trx replaces the desktop layout with the transceiver one: big TX and\nsound buttons, and the last minutes of the QSO kept for playback.");

        if (ImGui::Checkbox("Lock menu order##_sdrpp", &gui::menu.locked)) {
            core::configManager.acquire();
            core::configManager.conf["lockMenuOrder"] = gui::menu.locked;
            core::configManager.release(true);
        }
        helpMarker("Stops the sections of this menu being dragged into a different order.");

#ifdef __ANDROID__
        if (ImGui::Checkbox("Show battery##_sdrpp", &showBattery)) {
            gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);
            core::configManager.acquire();
            core::configManager.conf["showBattery"] = showBattery;
            core::configManager.release(true);
        }
#endif
#if 0
        if (ImGui::Checkbox("Detect Signals##_sdrpp", &detectSignals)) {
            sigpath::iqFrontEnd.togglePreprocessor(&sigpath::iqFrontEnd.detectorPreprocessor, detectSignals);
            core::configManager.acquire();
            core::configManager.conf["detectSignals"] = detectSignals;
            core::configManager.release(true);
        }
#endif

        ImGui::LeftLabel("Interface size");
        ImGui::FillWidth();
        if (ImGui::Combo("##sdrpp_ui_scale", &uiScaleId, uiScales.txt)) {
            core::configManager.acquire();
            core::configManager.conf["uiScale"] = uiScales[uiScaleId];
            core::configManager.release(true);
            restartRequired = true;
        }

        sectionHeader("SPECTRUM");

        // Each of these three is a switch plus the speed it runs at. Filling the rest
        // of the line with the number box left it too narrow for three digits once
        // the label and the (?) had taken their share, so 150 came out as 15. Wide
        // enough for four digits and the two step buttons, and no wider.
        float speedWidth = ImGui::CalcTextSize("8888").x + (ImGui::GetFrameHeight() * 2.0f) +
                           (ImGui::GetStyle().ItemInnerSpacing.x * 2.0f) + (ImGui::GetStyle().FramePadding.x * 2.0f);

        if (ImGui::Checkbox("Peak hold##_sdrpp", &fftHold)) {
            gui::waterfall.setFFTHold(fftHold);
            core::configManager.acquire();
            core::configManager.conf["fftHold"] = fftHold;
            core::configManager.release(true);
        }
        helpMarker("A second trace at the highest level each frequency has reached.\nThe number is how fast it falls back down.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(speedWidth);
        if (ImGui::InputInt("##sdrpp_fft_hold_speed", &fftHoldSpeed)) {
            // Was unclamped, unlike the two below it. Zero freezes the peak trace
            // where it is and a negative value walks it off the top of the chart.
            fftHoldSpeed = std::max<int>(fftHoldSpeed, 1);
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["fftHoldSpeed"] = fftHoldSpeed;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("Smoothing##_sdrpp", &fftSmoothing)) {
            gui::waterfall.setFFTSmoothing(fftSmoothing);
            core::configManager.acquire();
            core::configManager.conf["fftSmoothing"] = fftSmoothing;
            core::configManager.release(true);
        }
        helpMarker("Averages the spectrum over time, so noise stops flickering and weak\ncarriers stand out. The number is how fast it follows a change.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(speedWidth);
        if (ImGui::InputInt("##sdrpp_fft_smoothing_speed", &fftSmoothingSpeed)) {
            fftSmoothingSpeed = std::max<int>(fftSmoothingSpeed, 1);
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["fftSmoothingSpeed"] = fftSmoothingSpeed;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("SNR smoothing##_sdrpp", &snrSmoothing)) {
            gui::waterfall.setSNRSmoothing(snrSmoothing);
            core::configManager.acquire();
            core::configManager.conf["snrSmoothing"] = snrSmoothing;
            core::configManager.release(true);
        }
        helpMarker("Steadies the SNR meter beside the frequency readout, and everything\nthat reads it.");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(speedWidth);
        if (ImGui::InputInt("##sdrpp_snr_smoothing_speed", &snrSmoothingSpeed)) {
            snrSmoothingSpeed = std::max<int>(snrSmoothingSpeed, 1);
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["snrSmoothingSpeed"] = snrSmoothingSpeed;
            core::configManager.release(true);
        }

        if (ImGui::Checkbox("Redraw waterfall history on zoom##_sdrpp", &fullWaterfallUpdate)) {
            gui::waterfall.setFullWaterfallUpdate(fullWaterfallUpdate);
            core::configManager.acquire();
            core::configManager.conf["fullWaterfallUpdate"] = fullWaterfallUpdate;
            core::configManager.release(true);
        }
        helpMarker("On, zooming or panning redraws the whole waterfall to match. Off is cheaper\non a slow machine, but the history stays at the old zoom until it scrolls away.");

        sectionHeader("PROCESSING");

        ImGui::LeftLabel("Framerate");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::InputInt("##sdrpp_fft_rate", &fftRate, 1, 10)) {
            fftRate = std::max<int>(1, fftRate);
            sigpath::iqFrontEnd.setFFTRate(fftRate);
            updateFFTSpeeds();
            core::configManager.acquire();
            core::configManager.conf["fftRate"] = fftRate;
            core::configManager.release(true);
        }
        helpMarker("Spectrum updates per second. Lower costs less CPU and scrolls the\nwaterfall more slowly.");

        ImGui::LeftLabel("FFT size");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::Combo("##sdrpp_fft_size", &fftSizeId, FFTSizesStr)) {
            enableAcceleratedFFT = fftSizeId >= std::size(FFTSizes);
            sigpath::iqFrontEnd.setFFTSize(FFTSizes[fftSizeId % std::size(FFTSizes)]);
            core::configManager.acquire();
            core::configManager.conf["fftSize"] = FFTSizes[fftSizeId % std::size(FFTSizes)];
            core::configManager.conf["fftAccel"] = fftSizeId >= std::size(FFTSizes);
            core::configManager.release(true);
        }
        helpMarker("Bins the spectrum is split into. Bigger resolves closer signals apart\nand costs more CPU.");

        {
            static auto lastFFTReportTime = currentTimeMillis();
            static long long lastFFTReport = 0;
            auto ctm = currentTimeMillis();
            if (ctm - lastFFTReportTime > 1000) {
                lastFFTReportTime += 1000;
                lastFFTReport = fftCumulativeTime;
                fftCumulativeTime = 0;
            }
            // The bare "12 ms/s" this used to print sat next to the size box with
            // nothing to say what it measured.
            ImGui::TextDisabled("Costs %lld ms of CPU per second", lastFFTReport);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Time spent computing the spectrum, out of every second. 1000 would be\none core fully busy. Framerate and FFT size above are what move it.");
            }
        }

        ImGui::LeftLabel("Window");
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - (32.0f * style::uiScale));
        if (ImGui::Combo("##sdrpp_fft_window", &selectedWindow, "Rectangular\0Blackman\0Nuttall\0")) {
            sigpath::iqFrontEnd.setFFTWindow(fftWindowList[selectedWindow]);
            core::configManager.acquire();
            core::configManager.conf["fftWindow"] = selectedWindow;
            core::configManager.release(true);
        }
        helpMarker("Shapes each block before the FFT. Blackman and Nuttall stop a strong\nsignal smearing across its neighbours; Rectangular is sharpest but leaks.");

        sectionHeader("KEYBOARD AND MOUSE");

        // All of this already worked and none of it was written down anywhere, so
        // the only way to find out about it was to read the source.
        if (ImGui::TreeNode("Shortcuts##_sdrpp_shortcuts")) {
            if (ImGui::BeginTable("##_sdrpp_shortcut_table", 2, ImGuiTableFlags_SizingStretchProp)) {
                auto row = [](const char* keys, const char* what) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextUnformatted(keys);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextWrapped("%s", what);
                };
                row("End", "Start and stop the radio");
                row("Home", "Show and hide the waterfall");
                row("Page Up / Down", "Select the next or previous VFO");
                row("< >", "Over the spectrum: tune one snap step");
                row("Wheel", "Over the spectrum: tune. Shift for ten steps, Alt for a tenth");
                row("Wheel", "Over the frequency scale: pan the view");
                row("Ctrl", "Held over the spectrum: read the frequency under the pointer");
                row("Wheel", "Over a digit of the readout: change that digit");
                row("0-9", "Typed over a digit: enter a frequency from there on");
                row("Right click", "Over a digit: zero it and everything after it");
                row("Ctrl+C / Ctrl+V", "Over the readout: copy and paste the frequency");
                ImGui::EndTable();
            }
            ImGui::TreePop();
        }

        onDisplayDraw.emit(GImGui);

        if (restartRequired) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f), "Restart SDR++ to apply the interface size.");
        }
    }
}
