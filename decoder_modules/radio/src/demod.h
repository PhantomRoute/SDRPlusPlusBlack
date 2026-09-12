#pragma once
#include <dsp/stream.h>
#include <dsp/types.h>
#include <gui/widgets/waterfall.h>
#include <config.h>
#include <utils/event.h>
#include <gui/style.h>
#include <algorithm>
#include <string>


// The AGC is set by a rate: its coefficient is rate / sample rate, which is a one pole
// time constant of 1/rate seconds. As a number on a slider that means nothing to a
// listener, and the same number lands on a different time in each mode because the IF
// sample rates differ - 50 is 20 ms in AM but not in CW. So the slider works in
// milliseconds and converts. The config still holds the rate, so settings saved before
// this still load, and the DSP call is unchanged.
inline bool agcTimeSlider(const std::string& id, float& rate, float minMs, float maxMs) {
    float ms = (rate > 0.0f) ? (1000.0f / rate) : maxMs;
    ms = std::clamp(ms, minMs, maxMs);
    if (ImGui::SliderFloat(id.c_str(), &ms, minMs, maxMs, "%.0f ms", ImGuiSliderFlags_Logarithmic)) {
        rate = 1000.0f / std::clamp(ms, minMs, maxMs);
        return true;
    }
    return false;
}

enum DeemphasisMode {
    DEEMP_MODE_22US,
    DEEMP_MODE_50US,
    DEEMP_MODE_75US,
    DEEMP_MODE_NONE,
    _DEEMP_MODE_COUNT
};

enum IFNRPreset {
    IFNR_PRESET_NOAA_APT,
    IFNR_PRESET_VOICE,
    IFNR_PRESET_NARROW_BAND,
    IFNR_PRESET_BROADCAST
};

namespace demod {
    class Demodulator {
    public:
        virtual ~Demodulator() {}
        virtual void init(std::string name, ConfigManager* config, dsp::stream<dsp::complex_t>* input, double bandwidth, double audioSR) = 0;
        virtual void start() = 0;
        virtual void stop() = 0;
        virtual void showMenu() = 0;
        virtual void setBandwidth(double bandwidth) = 0;
        virtual void setInput(dsp::stream<dsp::complex_t>* input) = 0;
        virtual void AFSampRateChanged(double newSR) = 0;
        virtual const char* getName() = 0;
        virtual double getIFSampleRate() = 0;
        virtual double getAFSampleRate() = 0;
        virtual double getDefaultBandwidth() = 0;
        virtual double getMinBandwidth() = 0;
        virtual double getMaxBandwidth() = 0;
        virtual bool getBandwidthLocked() = 0;
        virtual double getDefaultSnapInterval() = 0;
        virtual int getVFOReference() = 0;
        virtual bool getDeempAllowed() = 0;
        virtual bool getPostProcEnabled() = 0;
        virtual int getDefaultDeemphasisMode() = 0;
        virtual bool getFMIFNRAllowed() = 0;
        virtual bool getNBAllowed() = 0;
        virtual void setFrozen(bool frozen) {};
        virtual dsp::stream<dsp::stereo_t>* getOutput() = 0;
    };
}

#include "demodulators/wfm.h"
#include "demodulators/nfm.h"
#include "demodulators/am.h"
#include "demodulators/usb.h"
#include "demodulators/lsb.h"
#include "demodulators/dsb.h"
#include "demodulators/cw.h"
#include "demodulators/raw.h"