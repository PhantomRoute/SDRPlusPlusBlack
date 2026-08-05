#pragma once
#include "../demod.h"

#include <dsp/demod/fm.h>
#include <dsp/correction/dc_blocker.h>
#include <dsp/buffer/packer.h>
#include <dsp/loop/agc.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/demod/psk.h>
#include <dsp/buffer/reshaper.h>
#include <dsp/routing/splitter.h>
#include <dsp/stream.h>
#include <dsp/sink/handler_sink.h>
#include <gui/widgets/constellation_diagram.h>
#include "../dsp/dsd.h"
#include "../dsp/slicer.h"
#include "dsd_status_ui.h"
#include "gui/style.h"
#include <dsp/clock_recovery/fd.h>
#include <dsp/clock_recovery/mm.h>
#include <chrono>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_map>

#define CONCAT(a, b) ((std::string(a) + b).c_str())

#define INSR                   (4800.0f * 2)
#define CLOCK_RECOVERY_BW      0.1f
#define CLOCK_RECOVERY_DAMPN_F 1.0f
#define CLOCK_RECOVERY_REL_LIM 0.001f
#define RRC_TAP_COUNT          65
#define RRC_ALPHA              0.23f

namespace demod {
    // P25p1 = 13 kHz
    // DStar = 7 kHz
    // NXDN48 = 7 kHz
    // NXDN96 = 13 kHz
    // ProVoice = ???(assumed 13 kHz)
    // DMR = 13 kHz
    // X2-TDMA = 13 kHz
    // DPMR = 7 kHz
    // YSF = 17 kHz
    class DSD : public Demodulator {
    public:
        struct DebugStatus {
            bool available = false;
            bool sync = false;
            bool dmr = false;
            bool voice = false;
            bool mbe_decoding = false;
            uint8_t color_code = 0;
            uint8_t slot0_burst = 0;
            uint8_t slot1_burst = 0;
            std::string slot0_type = "";
            std::string slot1_type = "";
            std::string mbe_errorbar = "";
        };

        DSD() {}

        DSD(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~DSD() {
            stop();
            unregisterInstance();
            dsp::taps::free(rrcTaps);
        }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            this->_config = config;
            registerInstance();

            // Load config
            _config->acquire();
            for (const auto& proto : protocols) {
                if (_config->conf[name][getName()].contains(proto.key)) {
                    bool enabled = _config->conf[name][getName()][proto.key];
                    decoder.*proto.flag = enabled ? 1 : 0;
                }
            }
            _config->release();

            // Define structure

            float recov_bandwidth = CLOCK_RECOVERY_BW;
            float recov_dampningFactor = CLOCK_RECOVERY_DAMPN_F;
            float recov_denominator = (1.0f + 2.0 * recov_dampningFactor * recov_bandwidth + recov_bandwidth * recov_bandwidth);
            float recov_mu = (4.0f * recov_dampningFactor * recov_bandwidth) / recov_denominator;
            float recov_omega = (4.0f * recov_bandwidth * recov_bandwidth) / recov_denominator;
            quadDemod.init(input, 1944.0f, INSR);
            dcBlock.init(&quadDemod.out, 1.0f / INSR);
            rrcTaps = dsp::taps::rootRaisedCosine<float>(RRC_TAP_COUNT, RRC_ALPHA, 4800.0f, INSR);
            rrcFilt.init(&dcBlock.out, rrcTaps);
            clockRecov.init(&rrcFilt.out, INSR / 4800.0f, recov_omega, recov_mu, CLOCK_RECOVERY_REL_LIM, 32, 8);
            constDiagSplitter.init(&clockRecov.out);
            constDiagSplitter.bindStream(&constDiagStream);
            constDiagSplitter.bindStream(&demodStream);

            constDiagReshaper.init(&constDiagStream, 1024, 0);
            constDiagSink.init(&constDiagReshaper.out, _constDiagSinkHandler, this);

            slicer.init(&demodStream);

            decoder.init(&slicer.out);
            outputConv.init(&decoder.out);
            outputMts.init(&outputConv.out);
        }

        DebugStatus getDebugStatus() {
            DebugStatus st;
            st.available = true;
            dsp::NewDSD::Frame_status fr_st = decoder.getFrameSyncStatus();
            dsp::NewDSD::MBE_status mbe_st = decoder.getMBEStatus();
            dsp::NewDSD::DMR_status dmr_st = decoder.getDMRStatus();
            st.sync = fr_st.sync;
            st.dmr = (fr_st.lasttype == dsp::NewDSD::Frame_status::LAST_DMR);
            st.mbe_decoding = mbe_st.mbe_status_decoding;
            st.voice = st.dmr && st.sync && st.mbe_decoding;
            st.color_code = dmr_st.dmr_status_cc;
            st.slot0_burst = dmr_st.dmr_status_s0_lastburstt;
            st.slot1_burst = dmr_st.dmr_status_s1_lastburstt;
            st.slot0_type = dmr_st.dmr_status_s0_lasttype;
            st.slot1_type = dmr_st.dmr_status_s1_lasttype;
            st.mbe_errorbar = mbe_st.mbe_status_errorbar;
            return st;
        }

        static bool getStatusForRadio(const std::string& radioName, DebugStatus& out) {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(radioName);
            if (it == instances.end() || !it->second) {
                return false;
            }
            out = it->second->getDebugStatus();
            return true;
        }

        static bool waitForDMRSyncVoice(const std::string& radioName, int stableMs, int timeoutMs, DebugStatus& out, int& waitedMs) {
            auto start = std::chrono::steady_clock::now();
            auto lastLoop = start;
            int accumulatedVoiceMs = 0;
            waitedMs = 0;

            while (true) {
                DebugStatus cur;
                if (!getStatusForRadio(radioName, cur)) {
                    out = DebugStatus{};
                    waitedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                    return false;
                }

                out = cur;
                auto now = std::chrono::steady_clock::now();
                int stepMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(now - lastLoop).count();
                lastLoop = now;
                if (cur.voice) {
                    accumulatedVoiceMs += stepMs;
                    if (accumulatedVoiceMs >= stableMs) {
                        waitedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                        return true;
                    }
                }

                waitedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
                if (waitedMs >= timeoutMs) {
                    if (accumulatedVoiceMs >= stableMs) { return true; }
                    return false;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }

        void start() {
            constDiagSplitter.start();
            quadDemod.start();
            dcBlock.start();
            rrcFilt.start();
            clockRecov.start();
            constDiagReshaper.start();
            constDiagSink.start();
            slicer.start();
            decoder.start();
            outputConv.start();
            outputMts.start();
        }

        void stop() {
            constDiagSplitter.stop();
            quadDemod.stop();
            dcBlock.stop();
            rrcFilt.stop();
            clockRecov.stop();
            constDiagReshaper.stop();
            constDiagSink.stop();
            slicer.stop();
            decoder.stop();
            outputConv.stop();
            outputMts.stop();
        }

        void showMenu() {
            float menuWidth = ImGui::GetContentRegionAvail().x;

            std::string protoSummary = getProtocolSummary();
            ImGui::LeftLabel("Protocols");
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
            if (ImGui::BeginCombo(("##_dsd_protocol_selector_" + name).c_str(), protoSummary.c_str())) {
                for (const auto& proto : protocols) {
                    bool enabled = decoder.*proto.flag == 1;
                    if (ImGui::Checkbox((std::string(proto.label) + "##_dsd_proto_" + name).c_str(), &enabled)) {
                        decoder.*proto.flag = enabled ? 1 : 0;
                        _config->acquire();
                        _config->conf[name][getName()][proto.key] = enabled;
                        _config->release(true);
                    }
                }
                ImGui::EndCombo();
            }
            if (protoSummary == "None") {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "No protocols selected");
            }

            ImGui::Spacing();

            drawLevelBar((int)(symbolPeak.load(std::memory_order_relaxed) * 100.0f), levelSmoothed);

            // The constellation sits under it: the bar says how strong, this says
            // how clean, by showing where the symbols land against the slicer levels.
            ImGui::LeftLabel("Signal");
            ImGui::SetNextItemWidth(menuWidth);
            constDiag.draw(ImVec2(0, 20));

            dsp::NewDSD::Frame_status fr_st = decoder.getFrameSyncStatus();
            const char* protoName = "-";
            if (fr_st.lasttype == dsp::NewDSD::Frame_status::LAST_P25) { protoName = "P25p1"; }
            else if (fr_st.lasttype == dsp::NewDSD::Frame_status::LAST_DMR) { protoName = "DMR"; }
            drawSyncLine(fr_st.sync, protoName);

            // Only the protocol actually being decoded, and only the fields worth
            // reading at a glance. The rest went behind Details rather than filling
            // the panel with hex that means nothing unless you are chasing something.
            if (!fr_st.sync) { style::beginDisabled(); }
            switch (fr_st.lasttype) {
            case dsp::NewDSD::Frame_status::LAST_P25: {
                dsp::NewDSD::P25_status p25_st = decoder.getP25Status();
                ImGui::Text("NAC  0x%03x   SRC %u", p25_st.p25_status_nac, p25_st.p25_status_src);
                ImGui::Text("TG   %u", p25_st.p25_status_tg);
                ImGui::Text("DUID %u%u %s", p25_st.p25_status_lastduid[0], p25_st.p25_status_lastduid[1], p25_st.p25_status_lasttype.c_str());
                if (p25_st.p25_status_irr_err) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Irrecoverable error");
                }
                // Worth calling out plainly - an encrypted call is the usual reason
                // for a solid sync producing no audio.
                if (p25_st.p25_status_algid != 0x80) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Encrypted (ALG 0x%02x, KEY 0x%04x)", p25_st.p25_status_algid, p25_st.p25_status_kid);
                }
                else {
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Unencrypted");
                }
                if (p25_st.p25_status_emr) {
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "Emergency");
                }
                if (ImGui::TreeNode(("Details##_dsd_p25_detail_" + name).c_str())) {
                    ImGui::Text("TGID     %u", p25_st.p25_status_tgid);
                    ImGui::Text("Other TG %u %u %u", p25_st.p25_status_othertg1, p25_st.p25_status_othertg2, p25_st.p25_status_othertg3);
                    ImGui::Text("MFID     0x%02x", p25_st.p25_status_mfid);
                    ImGui::Text("LCFORMAT 0x%02x", p25_st.p25_status_lcformat);
                    ImGui::Text("LCINFO   0x%016llx", (unsigned long long)p25_st.p25_status_lcinfo);
                    ImGui::Text("MI(INV)  0x%016llx %04x", (unsigned long long)p25_st.p25_status_mi_0, p25_st.p25_status_mi_1);
                    ImGui::TreePop();
                }
                break;
            }
            case dsp::NewDSD::Frame_status::LAST_DMR: {
                dsp::NewDSD::DMR_status dmr_st = decoder.getDMRStatus();
                ImGui::Text("Slot 0  (%02d) %s", dmr_st.dmr_status_s0_lastburstt, dmr_st.dmr_status_s0_lasttype.c_str());
                ImGui::Text("Slot 1  (%02d) %s", dmr_st.dmr_status_s1_lastburstt, dmr_st.dmr_status_s1_lasttype.c_str());
                ImGui::Text("Colour code 0x%02x", dmr_st.dmr_status_cc);
                break;
            }
            default:
                break;
            }
            if (!fr_st.sync) { style::endDisabled(); }

            dsp::NewDSD::MBE_status mbe_st = decoder.getMBEStatus();
            drawVoiceQualityBar(mbe_st.mbe_status_errorbar, fr_st.sync && mbe_st.mbe_status_decoding, voiceQualitySmoothed, name);
        }

        void setBandwidth(double bandwidth) {}

        void setInput(dsp::stream<dsp::complex_t>* input) {
            quadDemod.setInput(input);
        }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "DSD"; }
        double getIFSampleRate() { return INSR; }
        double getAFSampleRate() { return 8000.0; }
        double getDefaultBandwidth() { return bw; }
        double getMinBandwidth() { return 3000.0; }
        double getMaxBandwidth() { return 12500.0; }
        bool getBandwidthLocked() { return true; }
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
        void registerInstance() {
            std::lock_guard<std::mutex> lock(registryMtx);
            instances[name] = this;
        }

        void unregisterInstance() {
            std::lock_guard<std::mutex> lock(registryMtx);
            auto it = instances.find(name);
            if (it != instances.end() && it->second == this) {
                instances.erase(it);
            }
        }

        // Frame syncs the decoder will search for, in menu order. Everything
        // enabled means auto-detect. NXDN, D-STAR, X2-TDMA and ProVoice are not
        // implemented by this decoder - use the oldDSD mode for those.
        struct Protocol {
            const char* label;
            const char* key;
            int dsp::NewDSD::* flag;
        };

        inline static const Protocol protocols[] = {
            { "P25p1", "p25p1", &dsp::NewDSD::frameP25p1 },
            { "DMR", "dmr", &dsp::NewDSD::frameDmr }
        };

        std::string getProtocolSummary() {
            int enabledCount = 0;
            std::string summary;
            for (const auto& proto : protocols) {
                if (decoder.*proto.flag != 1) { continue; }
                enabledCount++;
                if (!summary.empty()) { summary += ", "; }
                summary += proto.label;
            }
            if (enabledCount == 0) { return "None"; }
            if (enabledCount == IM_ARRAYSIZE(protocols)) { return "Auto (P25p1, DMR)"; }
            return summary;
        }

        float bw = 12500.0;

        inline static std::mutex registryMtx;
        inline static std::unordered_map<std::string, DSD*> instances;

        dsp::demod::Quadrature quadDemod;
        dsp::correction::DCBlocker<float> dcBlock;
        dsp::filter::FIR<float, float> rrcFilt;
        dsp::tap<float> rrcTaps;
        dsp::clock_recovery::FD clockRecov;
        dsp::routing::Splitter<float> constDiagSplitter;
        dsp::stream<float> constDiagStream;
        dsp::buffer::Reshaper<float> constDiagReshaper;
        dsp::sink::Handler<float> constDiagSink;
        ImGui::ConstellationDiagram constDiag;

        dsp::stream<float> demodStream;
        dsp::FourFSKExtractor slicer;
        dsp::NewDSD decoder;
        dsp::Int16ToFloat outputConv;
        dsp::convert::MonoToStereo outputMts;

        std::string name;
        ConfigManager* _config = NULL;
        float voiceQualitySmoothed = 0.0f;
        float levelSmoothed = 0.0f;
        // Written by the DSP thread in _constDiagSinkHandler, read by the menu.
        std::atomic<float> symbolPeak { 0.0f };

        static void _constDiagSinkHandler(float* data, int count, void* ctx) {
            DSD* _this = (DSD*)ctx;

            // Track the symbol amplitude for the level bar. The slicer's own max and
            // min would be the obvious source, but the AFC that used to move them is
            // commented out, so they sit at their initial +-1 forever. These symbols
            // are the slicer's input, and quadDemod normalises 1944Hz of deviation to
            // 1.0, so a correctly tuned signal peaks around there and the reading
            // lands near 100%. Decays slowly so it holds through a gap between
            // bursts rather than dropping out with the carrier.
            float peak = 0.0f;
            for (int i = 0; i < count; i++) {
                float mag = data[i] < 0.0f ? -data[i] : data[i];
                if (mag > peak) { peak = mag; }
            }
            float prev = _this->symbolPeak.load(std::memory_order_relaxed);
            _this->symbolPeak.store(peak > prev ? peak : prev + ((peak - prev) * 0.15f),
                                    std::memory_order_relaxed);

            dsp::complex_t* cdBuff = _this->constDiag.acquireBuffer();
            if (count == 1024) {
                for (int i = 0; i < 1021; i++) {
                    cdBuff[i].re = data[i];
                    cdBuff[i].im = 0;
                }
                //                cdBuff[1019].re = _this->slicer.max;
                //                cdBuff[1019].im = 0.5f;
                //                cdBuff[1020].re = _this->slicer.min;
                //                cdBuff[1020].im = 0.5f;
                // Display slicer ranges too
                cdBuff[1021].re = _this->slicer.center;
                cdBuff[1021].im = 1.0f;
                cdBuff[1022].re = _this->slicer.umid;
                cdBuff[1022].im = 1.0f;
                cdBuff[1023].re = _this->slicer.lmid;
                cdBuff[1023].im = 1.0f;
            }
            _this->constDiag.releaseBuffer();
        }
    };
}
