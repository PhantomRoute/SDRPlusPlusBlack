#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <config.h>
#include <core.h>
#include <imgui/imgui_internal.h>
#include <vector>
#include <deque>
#include <cmath>
#include <algorithm>
#include "signal_path/signal_path.h"
#include <radio_interface.h>
#include "if_nr.h"
#include "af_nr.h"
#include <http_debug_server.h>

using namespace ImGui;

ConfigManager config;

// How much of the SNR history the chart keeps and draws, and how often it samples.
static const double SNR_CHART_SPAN = 60.0;
static const double SNR_SAMPLE_PERIOD = 0.05;


SDRPP_MOD_INFO{
    /* Name:            */ "noise_reduction_logmmse",
    /* Description:     */ "LOGMMSE noise reduction",
    /* Author:          */ "sannysanoff",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ -1
};

class NRModule : public ModuleManager::Instance {

    dsp::IFNRLogMMSE ifnrProcessor;

    std::unordered_map<std::string, std::shared_ptr<dsp::AFNRLogMMSE>> afnrProcessors;      // instance by radio name.
    std::unordered_map<std::string, std::shared_ptr<dsp::AFNR_OMLSA_MCRA>> afnrProcessors2; // instance by radio name.

public:
    NRModule(std::string name) {
        this->name = name;
        config.acquire();
        if (config.conf.contains("IFNR")) ifnr = config.conf["IFNR"];
        if (config.conf.contains("DisableCpuDeactivation")) disableCpuDeactivation = config.conf["DisableCpuDeactivation"];
        if (config.conf.contains("SNRChartWidget")) snrChartWidget = config.conf["SNRChartWidget"];
        config.release(true);

        ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        updateBindings();
        actuateIFNR();

        std::string path = "/modules/noise_reduction_logmmse/" + name;
        httpdebug::procfs::registerEndpoint(path + "/baseband_nr", [&]() { return ifnr ? "true" : "false"; }, [&](const std::string& v) { ifnr = (v == "true" || v == "1"); actuateIFNR(); }, httpdebug::procfs::Type::Bool);
        httpdebug::procfs::registerEndpoint(path + "/snr_chart", [&]() { return snrChartWidget ? "true" : "false"; }, [&](const std::string& v) { snrChartWidget = (v == "true" || v == "1"); }, httpdebug::procfs::Type::Bool);
        httpdebug::procfs::registerEndpoint(path + "/cpu_usage", [&]() { return std::to_string(ifnrProcessor.percentUsage); }, nullptr, httpdebug::procfs::Type::Int);
    }

    ~NRModule() {
        std::string path = "/modules/noise_reduction_logmmse/" + name;
        httpdebug::procfs::unregister(path + "/baseband_nr");
        httpdebug::procfs::unregister(path + "/snr_chart");
        httpdebug::procfs::unregister(path + "/cpu_usage");
        gui::menu.removeEntry(name);
        // Both of these keep a pointer to this instance: the event's handler list and
        // the bottom window's draw lambda. Neither was being dropped, so deleting the
        // module while it was enabled left the main window calling into freed memory.
        if (enabled) {
            gui::mainWindow.onWaterfallDrawn.unbindHandler(&waterfallDrawnHandler);
        }
        hideSNRChart();
    }

    void postInit() {}

    void enable() {
        if (!enabled) {
            enabled = true;
            ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);
            updateBindings();
            actuateIFNR();
        }
    }

    void disable() {
        if (enabled) {
            enabled = false;
            actuateIFNR();
            updateBindings();
        }
    }

    bool isEnabled() {
        return enabled;
    }


private:
    bool ifnr = false;
    bool disableCpuDeactivation = false;

    bool afnrEnabled = false;
    bool snrChartWidget = false;

    void attachAFToRadio(const std::string& instanceName) {
        auto afnrlogmmse = std::make_shared<dsp::AFNRLogMMSE>();
        afnrProcessors[instanceName] = afnrlogmmse;
        afnrlogmmse->init(nullptr);
        const std::shared_ptr<dsp::AFNR_OMLSA_MCRA> afnromlsa = std::make_shared<dsp::AFNR_OMLSA_MCRA>();
        afnromlsa->init(nullptr);
        afnrProcessors2[instanceName] = afnromlsa;
        core::modComManager.callInterface(instanceName, RADIO_IFACE_CMD_ADD_TO_IFCHAIN, afnrlogmmse.get(), NULL);
        core::modComManager.callInterface(instanceName, RADIO_IFACE_CMD_ADD_TO_AFCHAIN, afnromlsa.get(), NULL);
        core::modComManager.callInterface(instanceName, RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN, afnromlsa.get(), NULL);
        config.acquire();

        bool afnr = false;
        // if (config.conf.contains("AF_NR_"+instanceName)) afnr = config.conf["AF_NR_"+instanceName];
        auto frequency = 10;
        if (config.conf.contains("AF_NRF_" + instanceName)) frequency = config.conf["AF_NRF_" + instanceName];

        bool afnr2 = false;
        if (config.conf.contains("AF_NR2_" + instanceName)) afnr2 = config.conf["AF_NR2_" + instanceName];


        config.release(true);
        afnrlogmmse->afnrBandwidth = frequency;
        afnrlogmmse->setProcessingBandwidth(frequency * 1000);
        afnrlogmmse->allowed = afnr;

        afnromlsa->allowed = afnr2;

        actuateAFNR();
    }

    void detachAFFromRadio(const std::string& instanceName) {
        if (afnrProcessors.find(instanceName) != afnrProcessors.end()) {
            core::modComManager.callInterface(name, RADIO_IFACE_CMD_REMOVE_FROM_IFCHAIN,
                                              afnrProcessors[instanceName].get(), NULL);
            afnrProcessors.erase(instanceName);
        }
        if (afnrProcessors2.find(instanceName) != afnrProcessors2.end()) {
            core::modComManager.callInterface(name, RADIO_IFACE_CMD_REMOVE_FROM_AFCHAIN,
                                              afnrProcessors2[instanceName].get(), NULL);
            afnrProcessors2.erase(instanceName);
        }
    }

    void updateBindings() {
        if (enabled) {
            flog::info("Enabling noise reduction things");
            gui::mainWindow.onWaterfallDrawn.bindHandler(&waterfallDrawnHandler);
            waterfallDrawnHandler.ctx = this;
            waterfallDrawnHandler.handler = [](ImGuiContext* gctx, void* ctx) {
                NRModule* _this = (NRModule*)ctx;
                _this->tickSNRChart();
            };

            sigpath::iqFrontEnd.addPreprocessor(&ifnrProcessor, false);

            sigpath::sourceManager.onTuneChanged.bindHandler(&currentFrequencyChangedHandler);
            currentFrequencyChangedHandler.ctx = this;
            currentFrequencyChangedHandler.handler = [](double v, void* ctx) {
                auto _this = (NRModule*)ctx;
                _this->ifnrProcessor.reset(); // reset noise profile
            };

            auto names = core::modComManager.findInterfaces("radio");
            for (auto& name : names) {
                attachAFToRadio(name);
            }
            core::moduleManager.onInstanceCreated.bindHandler(&instanceCreatedHandler);
            instanceCreatedHandler.ctx = this;
            instanceCreatedHandler.handler = [](std::string v, void* ctx) {
                auto _this = (NRModule*)ctx;
                auto modname = core::moduleManager.getInstanceModuleName(v);
                if (modname == "radio") {
                    // radio created after the NR module.
                    _this->attachAFToRadio(v);
                    // on detach: module will remove pointer to AF NR from its chain, shared ptr will remain in the map until it gets replaced by a new one (maybe)
                }
                //
            };
        }
        else {
            sigpath::iqFrontEnd.removePreprocessor(&ifnrProcessor);
            gui::mainWindow.onWaterfallDrawn.unbindHandler(&waterfallDrawnHandler);
            // Nothing ticks once the handler is gone, so the chart would otherwise be
            // left on screen for good.
            hideSNRChart();
        }
    }


    std::unordered_map<std::string, long long> firstTimeHover;

    bool mustShowTooltip(const std::string& key) {
        if (ImGui::IsItemHovered()) {
            auto what = firstTimeHover[key];
            if (what == 0) {
                firstTimeHover[key] = currentTimeMillis();
                return false;
            }
            else {
                return currentTimeMillis() - what > 1000;
            }
        }
        else {
            firstTimeHover[key] = 0;
            return false;
        }
    }

    void actuateAFNR() {
        for (auto [k, v] : afnrProcessors) {
            core::modComManager.callInterface(k, !v->allowed ? RADIO_IFACE_CMD_DISABLE_IN_IFCHAIN : RADIO_IFACE_CMD_ENABLE_IN_IFCHAIN, v.get(), NULL);
        }
        //        for(auto [k, v] : afnrProcessors2) {
        //            core::modComManager.callInterface(k, !v->allowed ? RADIO_IFACE_CMD_DISABLE_IN_AFCHAIN : RADIO_IFACE_CMD_ENABLE_IN_AFCHAIN, v.get(), NULL);
        //        }
    }

    void actuateIFNR() {
        bool shouldRun = enabled && ifnr;
        if (ifnrProcessor.bypass != !shouldRun) {
            ifnrProcessor.bypass = !shouldRun;
            sigpath::iqFrontEnd.togglePreprocessor(&ifnrProcessor, shouldRun);
        }
    }

    void menuHandler() {
        float menuWidth = ImGui::GetContentRegionAvail().x;
        if (ImGui::Checkbox("Baseband NR##_sdrpp_if_nr", &ifnr)) {
            //            sigpath::signalPath.setWidebandNR(_this->widebandNR);
            config.acquire();
            config.conf["IFNR"] = ifnr;
            config.release(true);
            if (ifnr) { // toggled on - attempt to run.
                ifnrProcessor.stopReason = "";
            }
            actuateIFNR();
        }
        ImGui::SameLine();
        if (ifnrProcessor.stopReason != "" && ifnr) { // wants to stop -> stop it.
            ifnr = false;
            config.acquire();
            config.conf["IFNR"] = ifnr;
            config.release(true);
            actuateIFNR();
        }
        if (ifnrProcessor.stopReason != "") { // stopped because reason -> show it.
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0, 0, 1.0f));
            ImGui::Text("%s", ifnrProcessor.stopReason.c_str());
            ImGui::PopStyleColor(1);
        }
        else {
            // show cpu usage
            if (ifnrProcessor.percentUsage >= 0) {
                if (ifnrProcessor.percentUsage > 80) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0, 0, 1.0f));
                }
                std::string cpuText = std::to_string((int)ifnrProcessor.percentUsage) + "% cpu";
                ImVec2 textSize = ImGui::CalcTextSize(cpuText.c_str());
                bool clicked = ImGui::Selectable(cpuText.c_str(), false, ImGuiSelectableFlags_None, textSize);
                if (clicked) {
                    disableCpuDeactivation = !disableCpuDeactivation;
                    ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);
                    config.acquire();
                    config.conf["DisableCpuDeactivation"] = disableCpuDeactivation;
                    config.release(true);
                }
                if (disableCpuDeactivation) {
                    auto drawList = ImGui::GetWindowDrawList();
                    ImVec2 min = ImGui::GetItemRectMin();
                    ImVec2 max = ImGui::GetItemRectMax();
                    drawList->AddLine(ImVec2(min.x, (min.y + max.y) / 2), ImVec2(max.x, (min.y + max.y) / 2), ImGui::GetColorU32(ImGuiCol_Text), 1.0f);
                }
                if (ifnrProcessor.percentUsage > 80) {
                    ImGui::PopStyleColor(1);
                }
            }
        }

        for (auto [k, v] : afnrProcessors2) {
            if (ImGui::Checkbox(("Audio NR2 " + k + "##_radio_omlsa_nr_" + k).c_str(), &v->allowed)) {
                actuateAFNR();
                config.acquire();
                config.conf["AF_NR2_" + k] = v->allowed;
                config.release(true);
            }
            ImGui::SameLine();
            ImGui::Text("%0.01f", 32767.0 / v->scaled);
        }
        if (ImGui::Checkbox(("SNR chart##_radio_logmmse_nr_" + name).c_str(), &snrChartWidget)) {
            config.acquire();
            config.conf["SNRChartWidget"] = snrChartWidget;
            config.release(true);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Plots the SNR of the selected VFO over the last minute, in a panel\n"
                              "along the bottom of the window next to the audio waterfall. Useful\n"
                              "for telling whether the noise reduction above is actually helping.");
        }
    }

    static void menuHandler(void* ctx) {
        NRModule* _this = (NRModule*)ctx;
        _this->menuHandler();
    }

    // ---- SNR chart
    //
    // This used to be drawn straight over the waterfall as a bare polyline running
    // *down* the screen from wherever the SNR meter happened to sit: x was the
    // meter's pixel length rather than a dB value, y was the sample index, and there
    // was no scale, no units and no time base. It now lives in a window along the
    // bottom, beside the audio waterfall, and is plotted against dB and seconds.

    struct SnrSample {
        double time;
        float snr;
    };
    std::deque<SnrSample> snrHistory;
    double lastSnrSampleTime = 0.0;
    // Scratch for the per-column collapse below, kept between frames so drawing the
    // chart doesn't allocate on every one.
    std::vector<float> colSum;
    std::vector<int> colCount;
    std::vector<float> colValue;
    std::vector<float> colSmooth;
    std::vector<ImVec2> tracePoints;

    void showSNRChart() {
        if (gui::mainWindow.hasBottomWindow("snr_chart")) { return; }
        gui::mainWindow.addBottomWindow("snr_chart", [this]() { drawSNRChart(); });
    }

    void hideSNRChart() {
        if (!gui::mainWindow.hasBottomWindow("snr_chart")) { return; }
        gui::mainWindow.removeBottomWindow("snr_chart");
    }

    // Called once a frame from onWaterfallDrawn, which is also where the window has
    // to be added: the host draws the bottom windows later in the same frame.
    void tickSNRChart() {
        if (!snrChartWidget || !enabled) {
            hideSNRChart();
            snrHistory.clear();
            return;
        }
        showSNRChart();

        // The core's own measurement, in dB over the noise beside the channel. The
        // chart used to be fed the SNR meter's drawn pixel length instead, which is
        // scaled by the width of the meter and so means nothing on its own.
        double now = ImGui::GetTime();
        if ((now - lastSnrSampleTime) < SNR_SAMPLE_PERIOD) { return; }
        lastSnrSampleTime = now;

        if (gui::waterfall.selectedVFO.empty()) { return; }
        float snr = gui::waterfall.selectedVFOSNR;
        if (!std::isfinite(snr)) { return; }

        snrHistory.push_back({ now, snr });
        while (!snrHistory.empty() && (now - snrHistory.front().time) > SNR_CHART_SPAN) {
            snrHistory.pop_front();
        }
    }

    void drawSNRChart() {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 40.0f || avail.y < 30.0f) { return; }

        ImGui::PushFont(style::tinyFont);
        float labelWidth = ImGui::CalcTextSize("100").x + (4.0f * style::uiScale);
        ImGui::PopFont();
        // The readout line above the plot is drawn in the normal font, so the room
        // left for it has to be measured in that font.
        float headerHeight = ImGui::GetTextLineHeightWithSpacing();

        ImVec2 origin = ImGui::GetCursorScreenPos();
        ImVec2 plotMin(origin.x + labelWidth, origin.y + headerHeight);
        ImVec2 plotMax(origin.x + avail.x, origin.y + avail.y);
        if (plotMax.x - plotMin.x < 10.0f || plotMax.y - plotMin.y < 10.0f) { return; }

        ImDrawList* draw = ImGui::GetWindowDrawList();
        ImU32 gridColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
        ImU32 borderColor = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor);
        ImU32 textColor = ImGui::GetColorU32(ImGuiCol_Text);
        ImVec4 traceCol = gui::themeManager.snrMeterColor;

        // A fixed scale is easier to read at a glance than one that rescales itself
        // under the trace, but a signal that runs off the top is worse, so the top
        // only ever moves up to the next 10dB mark.
        float top = 40.0f;
        float latest = 0.0f;
        float lowest = INFINITY;
        float highest = -INFINITY;
        double sum = 0.0;
        for (const auto& s : snrHistory) {
            if (s.snr > highest) { highest = s.snr; }
            if (s.snr < lowest) { lowest = s.snr; }
            sum += s.snr;
        }
        if (!snrHistory.empty()) {
            latest = snrHistory.back().snr;
            // Round the top up to the next 10dB mark, and no further than 100: a
            // single wild reading should not squash the trace into the floor.
            top = std::clamp(ceilf(highest / 10.0f) * 10.0f, 40.0f, 100.0f);
        }

        double now = ImGui::GetTime();
        auto xOf = [&](double t) {
            double age = now - t;
            double f = 1.0 - (age / SNR_CHART_SPAN);
            return plotMin.x + (float)(std::clamp(f, 0.0, 1.0) * (plotMax.x - plotMin.x));
        };
        auto yOf = [&](float db) {
            float f = std::clamp(db / top, 0.0f, 1.0f);
            return plotMax.y - (f * (plotMax.y - plotMin.y));
        };

        // dB grid, labelled down the left
        ImGui::PushFont(style::tinyFont);
        for (float db = 0.0f; db <= top + 0.1f; db += 10.0f) {
            float y = yOf(db);
            draw->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), gridColor, 1.0f);
            char buf[16];
            snprintf(buf, sizeof buf, "%d", (int)db);
            ImVec2 sz = ImGui::CalcTextSize(buf);
            draw->AddText(ImVec2(plotMin.x - sz.x - (3.0f * style::uiScale), y - (sz.y / 2.0f)), textColor, buf);
        }
        // 10 second marks, so the width means something
        for (double t = 10.0; t < SNR_CHART_SPAN; t += 10.0) {
            float x = xOf(now - t);
            draw->AddLine(ImVec2(x, plotMin.y), ImVec2(x, plotMax.y), gridColor, 1.0f);
        }
        ImGui::PopFont();

        draw->AddRect(plotMin, plotMax, borderColor);

        // Collapse the samples into one value per pixel column before drawing any of
        // them. A minute at 20 samples a second is 1200 points across a plot a few
        // hundred pixels wide, so drawing a segment per sample put four sub-pixel
        // wide, anti-aliased quads into every column: the partial coverage of each
        // one is what streaked the fill with vertical lines. Averaging into columns
        // is also the honest way to show more data than there are pixels for.
        int cols = (int)(plotMax.x - plotMin.x);
        if (snrHistory.size() >= 2 && cols >= 2) {
            colSum.assign(cols, 0.0f);
            colCount.assign(cols, 0);
            for (const auto& s : snrHistory) {
                double f = 1.0 - ((now - s.time) / SNR_CHART_SPAN);
                int c = (int)((f * (double)(cols - 1)) + 0.5);
                if (c < 0 || c >= cols) { continue; }
                colSum[c] += s.snr;
                colCount[c]++;
            }

            // Carry the last value across columns no sample landed in - a dropped
            // frame, or the gap left while the radio was stopped - so the trace does
            // not fall to the floor and back between two real readings.
            colValue.assign(cols, 0.0f);
            int firstCol = -1;
            float carry = 0.0f;
            for (int c = 0; c < cols; c++) {
                if (colCount[c] > 0) {
                    carry = colSum[c] / (float)colCount[c];
                    if (firstCol < 0) { firstCol = c; }
                }
                colValue[c] = carry;
            }

            if (firstCol >= 0 && firstCol < cols - 1) {
                // One pass of a 3 tap average over the columns. Enough to take the
                // hard edge off without flattening anything worth seeing.
                colSmooth.assign(colValue.begin(), colValue.end());
                for (int c = firstCol + 1; c < cols - 1; c++) {
                    colSmooth[c] = (colValue[c - 1] + colValue[c] + colValue[c + 1]) / 3.0f;
                }

                tracePoints.clear();
                tracePoints.reserve(cols - firstCol);
                for (int c = firstCol; c < cols; c++) {
                    tracePoints.push_back(ImVec2(plotMin.x + (float)c, yOf(colSmooth[c])));
                }

                ImVec4 fillCol = traceCol;
                fillCol.w = 0.25f;
                ImU32 fill = ImGui::ColorConvertFloat4ToU32(fillCol);
                ImU32 trace = ImGui::ColorConvertFloat4ToU32(traceCol);

                // One quad per column, each a whole pixel wide. Anti-aliased filling
                // feathers half a pixel in from every edge, which on shapes this
                // narrow leaves each column edged with its own gradient and the
                // whole fill striped; the columns meet exactly without it.
                ImDrawListFlags fillFlags = draw->Flags;
                draw->Flags &= ~ImDrawListFlags_AntiAliasedFill;
                for (size_t i = 1; i < tracePoints.size(); i++) {
                    const ImVec2& a = tracePoints[i - 1];
                    const ImVec2& b = tracePoints[i];
                    draw->AddQuadFilled(a, b, ImVec2(b.x, plotMax.y), ImVec2(a.x, plotMax.y), fill);
                }
                draw->Flags = fillFlags;
                // And a single polyline over the top, which anti-aliases as one
                // continuous stroke instead of a few hundred separate ones.
                draw->AddPolyline(tracePoints.data(), (int)tracePoints.size(), trace, 0, 1.5f);
            }
        }

        // Readouts on the header line: what it is, where it is now, and what it has
        // been doing over the window.
        ImGui::SetCursorScreenPos(origin);
        if (snrHistory.empty()) {
            ImGui::TextDisabled("SNR   waiting for a VFO");
        }
        else {
            ImGui::TextColored(traceCol, "SNR %.1f dB", latest);
            ImGui::SameLine();
            ImGui::PushFont(style::tinyFont);
            ImGui::TextDisabled("min %.0f  avg %.0f  max %.0f  over %ds",
                                lowest, sum / (double)snrHistory.size(), highest, (int)SNR_CHART_SPAN);
            ImGui::PopFont();
        }
    }


    std::string name;
    bool enabled = true;
    EventHandler<ImGuiContext*> waterfallDrawnHandler;
    EventHandler<double> currentFrequencyChangedHandler;
    EventHandler<std::string> instanceCreatedHandler;
};


MOD_EXPORT void _INIT_() {
    config.setPath(std::string(core::getRoot()) + "/noise_reduction_logmmse_config.json");
    config.load(json::object());
    config.enableAutoSave();
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new NRModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (NRModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}