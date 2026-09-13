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
        // The SNR chart moved to Display > Panels, and its switch to the core config.
        // Carry a chart someone had turned on across, then drop the old key.
        if (config.conf.contains("SNRChartWidget")) {
            bool wasShown = config.conf["SNRChartWidget"];
            config.conf.erase("SNRChartWidget");
            core::configManager.acquire();
            if (wasShown && !core::configManager.conf.contains("showSNRChart")) {
                core::configManager.conf["showSNRChart"] = true;
            }
            core::configManager.release(true);
        }
        config.release(true);

        ifnrProcessor.setDisableCpuDeactivation(disableCpuDeactivation);

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        updateBindings();
        actuateIFNR();

        std::string path = "/modules/noise_reduction_logmmse/" + name;
        httpdebug::procfs::registerEndpoint(path + "/baseband_nr", [&]() { return ifnr ? "true" : "false"; }, [&](const std::string& v) { ifnr = (v == "true" || v == "1"); actuateIFNR(); }, httpdebug::procfs::Type::Bool);
        httpdebug::procfs::registerEndpoint(path + "/cpu_usage", [&]() { return std::to_string(ifnrProcessor.percentUsage); }, nullptr, httpdebug::procfs::Type::Int);
    }

    ~NRModule() {
        std::string path = "/modules/noise_reduction_logmmse/" + name;
        httpdebug::procfs::unregister(path + "/baseband_nr");
        httpdebug::procfs::unregister(path + "/cpu_usage");
        gui::menu.removeEntry(name);
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
    }

    static void menuHandler(void* ctx) {
        NRModule* _this = (NRModule*)ctx;
        _this->menuHandler();
    }



    std::string name;
    bool enabled = true;
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