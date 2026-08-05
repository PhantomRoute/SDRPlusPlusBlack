#pragma once
#include "../demod.h"
#include <dsp/demod/fm.h>
#include <dsp/correction/dc_blocker.h>
#include <dsp/loop/agc.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/demod/psk.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/routing/splitter.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <gui/widgets/constellation_diagram.h>
#include <gui/style.h>
#include "../dsp/dsd.h"
#include "dsd_status_ui.h"
#include <dsp/clock_recovery/fd.h>
#include <dsp/clock_recovery/mm.h>
#include <cstdio>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

namespace demod {
    //P25p1 = 13 kHz
    //DStar = 7 kHz
    //NXDN48 = 7 kHz
    //NXDN96 = 13 kHz
    //ProVoice = ???(assumed 13 kHz)
    //DMR = 13 kHz
    //X2-TDMA = 13 kHz
    //DPMR = 7 kHz
    //YSF = 17 kHz
    class OldDSD : public Demodulator {
    public:
        OldDSD() {}

        OldDSD(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~OldDSD() {
            stop();
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            this->_config = config;

            // Load config
            _config->acquire();
            for (const auto& proto : protocols) {
                if (_config->conf[name][getName()].contains(proto.key)) {
                    bool enabled = _config->conf[name][getName()][proto.key];
                    dsdDec.*proto.flag = enabled ? 1 : 0;
                }
            }
            if (_config->conf[name][getName()].contains("nxdnSyncTolerance")) {
                dsdDec.nxdnSyncTolerance = _config->conf[name][getName()]["nxdnSyncTolerance"];
            }
            if (_config->conf[name][getName()].contains("trackLevelsForGfsk")) {
                dsdDec.trackLevelsForGfsk = _config->conf[name][getName()]["trackLevelsForGfsk"];
            }
            _config->release();

            dsdDec.setDemodMode(getSelectedMode());

            // Define structure

            fmdemod.init(input, getIFSampleRate(), bandwidth, true, false);
            inputDcBlock.init(&fmdemod.out, 100.0f / getIFSampleRate());
            inputConv.init(&inputDcBlock.out);
            inputPacker.init(&inputConv.out, 120);
            dsdDec.init(&inputPacker.out);
            outputConv.init(&dsdDec.out);
            outputMts.init(&outputConv.out);
        }

        void start() {
            fmdemod.start();
            inputDcBlock.start();
            inputConv.start();
            inputPacker.start();
            dsdDec.start();
            outputConv.start();
            outputMts.start();
        }

        void stop() {
            fmdemod.stop();
            inputDcBlock.stop();
            inputConv.stop();
            inputPacker.stop();
            dsdDec.stop();
            outputConv.stop();
            outputMts.stop();
        }

        void showMenu() {
            std::string protoSummary = getProtocolSummary();
            ImGui::LeftLabel("Protocols");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo(("##_olddsd_protocol_selector_" + name).c_str(), protoSummary.c_str())) {
                for (const auto& proto : protocols) {
                    bool enabled = dsdDec.*proto.flag == 1;
                    if (ImGui::Checkbox((std::string(proto.label) + "##_olddsd_proto_" + name).c_str(), &enabled)) {
                        setProtocolEnabled(proto, enabled);
                        saveProtocols();
                    }
                }
                ImGui::EndCombo();
            }
            if (protoSummary == "None") {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No protocols selected");
            }
            if (getSelectedMode() == dsp::DSD::MODE_AUTO) {
                for (const auto& proto : protocols) {
                    if (dsdDec.*proto.flag != 1 || !proto.ownRate) { continue; }
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "Untick the rest to decode %s", proto.label);
                }
            }
            ImGui::Spacing();

            // Input level as a bar rather than a bare percentage - the useful
            // question is "is it in range", which a number makes you work out.
            drawLevelBar();

            drawSyncLine(dsdDec.status_sync, dsdDec.status_last_proto);

            // Only the protocol actually being decoded. The old panel listed P25, DMR
            // and NXDN at once, so two thirds of it was always stale zeroes in red.
            const std::string& proto = dsdDec.status_last_proto;
            if (proto.find("P25") != std::string::npos) {
                ImGui::Text("NAC %d   SRC %d   TG %d", dsdDec.status_last_nac, dsdDec.status_last_src, dsdDec.status_last_tg);
                if (!dsdDec.status_last_p25_duid.empty()) { ImGui::Text("DUID %s", dsdDec.status_last_p25_duid.c_str()); }
            } else if (proto.find("DMR") != std::string::npos) {
                ImGui::Text("Slot 0  %s", dsdDec.status_last_dmr_slot0_burst.c_str());
                ImGui::Text("Slot 1  %s", dsdDec.status_last_dmr_slot1_burst.c_str());
            } else if (proto.find("NXDN") != std::string::npos) {
                ImGui::Text("Frame   %s", dsdDec.status_last_nxdn_type.c_str());
            }

            drawVoiceQuality();

            const char* modNames[] = { "C4FM", "QPSK", "GFSK" };
            int rfMod = dsdDec.getRfMod();
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "%s  %d sps  sync %s",
                               (rfMod >= 0 && rfMod <= 2) ? modNames[rfMod] : "?",
                               dsdDec.getSamplesPerSymbol(),
                               getSyncTypeName(dsdDec.getLastSyncType()));

            // Tuning knobs for the NXDN work, collapsed by default and only shown
            // when they apply - the same way the main window tucks its debug section
            // away. Nobody needs these to listen to a channel.
            if (dsdDec.frameNxdn48 == 1 || dsdDec.frameNxdn96 == 1) {
                ImGui::Spacing();
                if (ImGui::CollapsingHeader(("Advanced##_olddsd_adv_" + name).c_str())) {
                    // Off is szechyjs's exact compare, on is what dsdcc and dsd-fme do.
                    // Worth turning off if it starts syncing on noise.
                    bool tolerant = dsdDec.nxdnSyncTolerance > 0;
                    if (ImGui::Checkbox(("Tolerant NXDN sync##_olddsd_nxdntol_" + name).c_str(), &tolerant)) {
                        dsdDec.nxdnSyncTolerance = tolerant ? 1 : 0;
                        _config->acquire();
                        _config->conf[name][getName()]["nxdnSyncTolerance"] = dsdDec.nxdnSyncTolerance;
                        _config->release(true);
                    }
                    // Off is upstream's behaviour: slicer levels frozen at whatever
                    // they were when the frame synced.
                    if (ImGui::Checkbox(("Track levels (GFSK)##_olddsd_gfsktrack_" + name).c_str(), &dsdDec.trackLevelsForGfsk)) {
                        _config->acquire();
                        _config->conf[name][getName()]["trackLevelsForGfsk"] = dsdDec.trackLevelsForGfsk;
                        _config->release(true);
                    }
                }
            }
        }

        void drawLevelBar() {
            float level = (float)dsdDec.status_lvl / 100.0f;
            if (level > 1.0f) { level = 1.0f; }
            if (level < 0.0f) { level = 0.0f; }
            levelSmoothed = approachValue(levelSmoothed, level, 10.0f);

            // Too quiet starves the slicer, pinned at the top means clipping.
            ImVec4 color = ImVec4(0.4f, 0.8f, 0.4f, 1.0f);
            if (dsdDec.status_lvl < 15) { color = ImVec4(1.0f, 0.6f, 0.3f, 1.0f); }
            else if (dsdDec.status_lvl > 95) { color = ImVec4(1.0f, 0.4f, 0.3f, 1.0f); }

            char overlay[32];
            snprintf(overlay, sizeof(overlay), "%d%%", dsdDec.status_lvl);
            ImGui::LeftLabel("Level");
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar(levelSmoothed, ImVec2(ImGui::GetContentRegionAvail().x, 0), overlay);
            ImGui::PopStyleColor();
        }

        void drawVoiceQuality() {
            drawVoiceQualityBar(dsdDec.status_errorbar, dsdDec.status_sync, voiceQualitySmoothed, name);
        }

        // Names getFrameSync's return codes. A protocol showing up here without
        // "Mode:" filling in means its sync pattern was matched once but the
        // second, confirming match never arrived.
        static const char* getSyncTypeName(int syncType) {
            switch (syncType) {
                case 0: case 1:   return "P25p1";
                case 2: case 3:
                case 4: case 5:   return "X2-TDMA";
                case 6: case 7:   return "D-STAR";
                case 8: case 9:   return "NXDN voice";
                case 10: case 11:
                case 12: case 13: return "DMR";
                case 14: case 15: return "ProVoice";
                case 16: case 17: return "NXDN data";
                case 18: case 19: return "D-STAR HD";
                default:          return "none";
            }
        }

        void setBandwidth(double bandwidth) {
            fmdemod.setBandwidth(bandwidth);
        }

        void setInput(dsp::stream<dsp::complex_t>* input) {
            fmdemod.setInput(input);
        }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "OldDSD"; }
        double getIFSampleRate() { return 48000.0; }
        double getAFSampleRate() { return 8000.0; }
        double getDefaultBandwidth() { return bw; }
        double getMinBandwidth() { return 3000.0; }
        double getMaxBandwidth() { return 12500.0; }
        bool getBandwidthLocked() { return false; }
        double getMaxAFBandwidth() { return 4000.0; }
        double getDefaultSnapInterval() { return 500.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        double getAFBandwidth(double bandwidth) { return 4000.0; }
        bool getDynamicAFBandwidth() { return false; }
        bool getFMIFNRAllowed() { return false; }
        bool getNBAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &outputMts.out; }

    private:

        // Frame sync detectors the decoder will attempt, in menu order. Protocols
        // with a mode other than MODE_AUTO reconfigure the demodulator, so they
        // only get that configuration when nothing else is selected - the same
        // trade upstream dsd makes between its default search set and -fi/-fn/-fp.
        // ownRate marks the ones that additionally change the symbol rate, which
        // means they cannot sync at all unless they are the only selection.
        struct Protocol {
            const char* label;
            const char* key;
            int dsp::DSD::* flag;
            dsp::DSD::DemodMode mode;
            bool ownRate;
        };

        inline static const Protocol protocols[] = {
            { "P25p1", "p25p1", &dsp::DSD::frameP25p1, dsp::DSD::MODE_AUTO, false },
            { "ProVoice", "provoice", &dsp::DSD::frameProvoice, dsp::DSD::MODE_PROVOICE, true },
            { "X2-TDMA", "x2tdma", &dsp::DSD::frameX2tdma, dsp::DSD::MODE_AUTO, false },
            { "DMR", "dmr", &dsp::DSD::frameDmr, dsp::DSD::MODE_AUTO, false },
            { "NXDN48", "nxdn48", &dsp::DSD::frameNxdn48, dsp::DSD::MODE_NXDN48, true },
            { "NXDN96", "nxdn96", &dsp::DSD::frameNxdn96, dsp::DSD::MODE_NXDN96, false },
            { "D-STAR", "dstar", &dsp::DSD::frameDstar, dsp::DSD::MODE_AUTO, false }
        };

        // The pinned protocol's mode when exactly one is selected, MODE_AUTO
        // otherwise.
        dsp::DSD::DemodMode getSelectedMode() {
            const Protocol* only = NULL;
            int enabledCount = 0;
            for (const auto& proto : protocols) {
                if (dsdDec.*proto.flag != 1) { continue; }
                enabledCount++;
                only = &proto;
            }
            if (enabledCount == 1) { return only->mode; }
            return dsp::DSD::MODE_AUTO;
        }

        void setProtocolEnabled(const Protocol& proto, bool enabled) {
            dsdDec.*proto.flag = enabled ? 1 : 0;
            dsdDec.setDemodMode(getSelectedMode());
        }

        void saveProtocols() {
            _config->acquire();
            for (const auto& proto : protocols) {
                _config->conf[name][getName()][proto.key] = (dsdDec.*proto.flag == 1);
            }
            _config->release(true);
        }

        std::string getProtocolSummary() {
            int enabledCount = 0;
            std::string summary;
            for (const auto& proto : protocols) {
                if (dsdDec.*proto.flag != 1) { continue; }
                enabledCount++;
                if (!summary.empty()) { summary += ", "; }
                summary += proto.label;
            }
            if (enabledCount == 0) { return "None"; }
            if (enabledCount == IM_ARRAYSIZE(protocols)) { return "All"; }
            return summary;
        }

        float bw = 12500.0;

        dsp::stream<dsp::complex_t> nullStream;
        dsp::demod::FM<float> fmdemod;
        dsp::correction::DCBlocker<float> inputDcBlock;
        dsp::FloatToInt16 inputConv;
        dsp::buffer::Packer<int16_t> inputPacker;
        dsp::DSD dsdDec;
        dsp::Int16ToFloat outputConv;
        dsp::convert::MonoToStereo outputMts;

        std::string name;
        ConfigManager* _config = NULL;
        float levelSmoothed = 0.0f;
        float voiceQualitySmoothed = 0.0f;

    };
}
