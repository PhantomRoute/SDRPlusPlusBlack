#pragma once
#include "../demod.h"
#include <dsp/demod/am.h>

namespace demod {
    class AM : public Demodulator {
    public:
        AM() {}

        AM(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            init(name, config, input, bandwidth, audioSR);
        }

        ~AM() { stop(); }

        void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) {
            this->name = name;
            _config = config;

            // Load config
            config->acquire();
            if (config->conf[name][getName()].contains("agcAttack")) {
                agcAttack = config->conf[name][getName()]["agcAttack"];
            }
            if (config->conf[name][getName()].contains("agcDecay")) {
                agcDecay = config->conf[name][getName()]["agcDecay"];
            }
            if (config->conf[name][getName()].contains("carrierAgc")) {
                carrierAgc = config->conf[name][getName()]["carrierAgc"];
            }
            config->release();

            // Define structure
            demod.init(input, carrierAgc ? dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::AM<dsp::stereo_t>::AGCMode::AUDIO, bandwidth, agcAttack / getIFSampleRate(), agcDecay / getIFSampleRate(), 100.0 / getIFSampleRate(), getIFSampleRate());
        }

        void start() { demod.start(); }

        void stop() { demod.stop(); }

        void showMenu() {
            float menuWidth = ImGui::GetContentRegionAvail().x;
            ImGui::LeftLabel("AGC Attack");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX() - (32.0f * style::uiScale));
            if (agcTimeSlider("##_radio_am_agc_attack_" + name, agcAttack, 5.0f, 1000.0f)) {
                demod.setAGCAttack(agcAttack / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcAttack"] = agcAttack;
                _config->release(true);
            }
            ImGui::HelpMarker("How fast the gain comes down when a signal gets louder. Short keeps sudden peaks in check.");
            ImGui::LeftLabel("AGC Decay");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX() - (32.0f * style::uiScale));
            if (agcTimeSlider("##_radio_am_agc_decay_" + name, agcDecay, 50.0f, 1000.0f)) {
                demod.setAGCDecay(agcDecay / getIFSampleRate());
                _config->acquire();
                _config->conf[name][getName()]["agcDecay"] = agcDecay;
                _config->release(true);
            }
            ImGui::HelpMarker("How fast the gain comes back up in the gaps. Long stops the noise being pumped up between words; never faster than the attack.");
            if (ImGui::Checkbox(("Carrier AGC##_radio_am_carrier_agc_" + name).c_str(), &carrierAgc)) {
                demod.setAGCMode(carrierAgc ? dsp::demod::AM<dsp::stereo_t>::AGCMode::CARRIER : dsp::demod::AM<dsp::stereo_t>::AGCMode::AUDIO);
                _config->acquire();
                _config->conf[name][getName()]["carrierAgc"] = carrierAgc;
                _config->release(true);
            }
        }

        void setBandwidth(double bandwidth) { demod.setBandwidth(bandwidth); }

        void setInput(dsp::stream<dsp::complex_t>* input) { demod.setInput(input); }

        void AFSampRateChanged(double newSR) {}

        // ============= INFO =============

        const char* getName() { return "AM"; }
        double getIFSampleRate() { return 15000.0; }
        double getAFSampleRate() { return getIFSampleRate(); }
        double getDefaultBandwidth() { return 10000.0; }
        double getMinBandwidth() { return 1000.0; }
        // Comfortably past a 9 or 10 kHz broadcast channel. Was getIFSampleRate(),
        // which unlocking also raises the ceiling to, so the switch did nothing here.
        // 12 kHz locked, 15 kHz unlocked.
        double getMaxBandwidth() { return 12000.0; }
        bool getBandwidthLocked() { return false; }
        double getDefaultSnapInterval() { return 1000.0; }
        int getVFOReference() { return ImGui::WaterfallVFO::REF_CENTER; }
        bool getDeempAllowed() { return false; }
        bool getPostProcEnabled() { return true; }
        int getDefaultDeemphasisMode() { return DEEMP_MODE_NONE; }
        bool getFMIFNRAllowed() { return false; }
        virtual void setAGCFrozen(bool frozen) { demod.setAGCFrozen(frozen); };
        bool getNBAllowed() { return false; }
        dsp::stream<dsp::stereo_t>* getOutput() { return &demod.out; }

    private:
        dsp::demod::AM<dsp::stereo_t> demod;

        ConfigManager* _config = NULL;

        float agcAttack = 50.0f;
        float agcDecay = 5.0f;
        bool carrierAgc = false;

        std::string name;
    };
}