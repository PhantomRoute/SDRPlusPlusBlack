#pragma once

#include <imgui.h>   // ImVec2, used below. Included here rather than relied on
                     // arriving through signal_path.h, so this header stands alone.
#include <module.h>
#include <dsp/demod/fm.h>
#include <dsp/multirate/rational_resampler.h>
#include <signal_path/signal_path.h>
#include <utils/cty.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "decoder.hpp"

// A radiosonde is a disposable weather station on a balloon: it climbs to around
// 30 km twice a day from a few hundred sites, sends its position and its pressure,
// temperature and humidity in the clear the whole way up, and then falls back down
// under a parachute. This module tunes one, decodes it, and says where it is.
//
// The decoding is sondedump (see ../sondedump/README.md), which handles the seven
// families of sonde and their frame formats. Everything in this file is the part
// around it: the DSP path from the VFO to the decoder, and what the panel shows.

// Everything the decoder can tell us, plus where and when we heard it.
struct SondeFix {
    // Milliseconds from currentTimeMillis(), not ImGui::GetTime(). These are stamped
    // on the decoder's worker thread, and ImGui's clock is part of a context that
    // belongs to the UI thread - reading it from anywhere else is a race on ImGui's
    // own state for a number a plain clock gives safely.
    long long time = 0;
    float lat = 0.0f;
    float lon = 0.0f;
    float alt = 0.0f;
};

class RadiosondeDecoderModule : public ModuleManager::Instance {
public:
    RadiosondeDecoderModule(std::string name);
    ~RadiosondeDecoderModule();

    void postInit() override;
    void enable() override;
    void disable() override;
    bool isEnabled() override;

private:
    // Display name, the channel width it needs, and the decoder that reads it.
    struct SondeType {
        const char* name;
        float bandwidth;
        dsp::block* decoder;
    };

    static void menuHandler(void* ctx);
    static void sondeDataHandler(SondeFullData* data, void* ctx);

    void selectType(int type);
    void startDSP();
    void stopDSP();

    void drawTelemetry();
    void drawTrack();
    void drawLogging();

    // Where the sonde is from where you are, using the grid square from the Source
    // menu. Nothing to configure: if the grid is set this fills in, if not it says so.
    // Takes the frame it should work from rather than reading lastData, which belongs
    // to the decoder thread and must only be read under the lock.
    static bool bearingFromOperator(const SondeFullData& d, utils::BearingDistance& bd,
                                    double& elevationDeg);

    bool openLog();
    void closeLog();
    void writeLogPoint(const SondeFullData& d);

    std::string name;
    bool enabled = true;

    VFOManager::VFO* vfo = NULL;
    dsp::demod::FM<float> fmDemod;
    dsp::multirate::RationalResampler<float> resampler;
    bool dspRunning = false;

    radiosonde::Decoder<RS41Decoder, rs41_decoder_init, rs41_decoder_deinit, rs41_decode> rs41decoder;
    radiosonde::Decoder<DFM09Decoder, dfm09_decoder_init, dfm09_decoder_deinit, dfm09_decode> dfm09decoder;
    radiosonde::Decoder<IMS100Decoder, ims100_decoder_init, ims100_decoder_deinit, ims100_decode> ims100decoder;
    radiosonde::Decoder<M10Decoder, m10_decoder_init, m10_decoder_deinit, m10_decode> m10decoder;
    radiosonde::Decoder<IMET4Decoder, imet4_decoder_init, imet4_decoder_deinit, imet4_decode> imet4decoder;
    radiosonde::Decoder<C50Decoder, c50_decoder_init, c50_decoder_deinit, c50_decode> c50decoder;
    radiosonde::Decoder<MRZN1Decoder, mrzn1_decoder_init, mrzn1_decoder_deinit, mrzn1_decode> mrzn1decoder;

    SondeType types[7] = {
        { "RS41 (Vaisala)", 10000.0f, &rs41decoder },
        { "DFM06/09 (GRAW)", 15000.0f, &dfm09decoder },
        { "iMS-100 / RS-11G (Meisei)", 20000.0f, &ims100decoder },
        { "M10 / M20 (Meteomodem)", 50000.0f, &m10decoder },
        { "iMet-1/4 (InterMet)", 20000.0f, &imet4decoder },
        { "SRS-C50 (Meteolabor)", 20000.0f, &c50decoder },
        { "MRZ-N1 (Meteo-Radiy)", 20000.0f, &mrzn1decoder },
    };
    int selectedType = 0;
    dsp::block* activeDecoder = NULL;

    // The decoder callback runs on its own worker thread, so everything it touches
    // that the panel also reads is behind this.
    std::mutex dataMtx;
    SondeFullData lastData;
    bool everDecoded = false;
    long long lastFrameTime = 0;   // currentTimeMillis(), see SondeFix::time
    int framesDecoded = 0;
    std::deque<SondeFix> track;
    // Scratch for the plot, kept between frames so drawing does not allocate on
    // every one. Only ever touched on the UI thread.
    std::vector<ImVec2> trackPoints;

    // GPX logging, written as it decodes rather than at the end, so pulling the
    // power out halfway through still leaves a usable file.
    bool logEnabled = false;
    char logPath[1024] = { 0 };
    FILE* logFile = NULL;
    std::string logStatus;
};
