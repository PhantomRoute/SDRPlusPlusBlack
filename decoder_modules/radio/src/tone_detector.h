#pragma once
#include <dsp/processor.h>
#include <dsp/types.h>
#include <dsp/multirate/rational_resampler.h>
#include <fftw3.h>
#include <volk/volk.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// Identifies the CTCSS tone or DCS code riding under demodulated FM audio.
//
// Both live below the voice band, so a single decimation to 672 S/s feeds both
// halves: 672 is above twice the highest CTCSS tone (254.1 Hz) and is exactly
// five samples per DCS bit (134.4 bps).
//
// The DCS code table and the Golay word check come from the CTCSS/DCS squelch in
// decoder_modules/ch_extravhf_decoder, which credits http://yo3iiu.ro/blog/?p=779,
// Bogdan Diaconescu <yo3iiu@yo3iiu.ro>.
namespace tonedetect {

    struct Result {
        enum Kind {
            NONE,
            CTCSS,
            DCS
        };

        Kind kind = NONE;
        float ctcssFreq = 0.0f; // Hz, snapped to the standard tone
        int dcsNormal = 0;      // the code a radio set to normal polarity would send
        int dcsInverted = 0;    // the same waveform read as an inverted code

        bool operator==(const Result& o) const {
            return kind == o.kind && ctcssFreq == o.ctcssFreq && dcsNormal == o.dcsNormal;
        }

        // The badge drawn over the waterfall. Short enough not to crowd the spectrum
        // on a phone.
        std::string label() const {
            char buf[64];
            switch (kind) {
            case CTCSS:
                snprintf(buf, sizeof buf, "CTCSS %.1f Hz", ctcssFreq);
                return buf;
            case DCS:
                snprintf(buf, sizeof buf, "DCS D%03d", dcsNormal);
                return buf;
            default:
                return "";
            }
        }

        // The readout in the radio menu, where there is room to spell it out. A DCS
        // word and its complement are the same waveform read two ways, so both
        // codes are always valid and only the operator knows which one their radio
        // calls it - showing one and hiding the other would be a guess.
        std::string longLabel() const {
            char buf[96];
            switch (kind) {
            case CTCSS:
                snprintf(buf, sizeof buf, "CTCSS %.1f Hz", ctcssFreq);
                return buf;
            case DCS:
                snprintf(buf, sizeof buf, "DCS D%03dN (or D%03dI)", dcsNormal, dcsInverted);
                return buf;
            default:
                return "Listening...";
            }
        }
    };

    // The 52 tones every radio that does CTCSS agrees on: EIA/TIA-603 plus the
    // interstitials. 150.0 and 151.4 are only 1.4 Hz apart, which is what sets the
    // snapping tolerance below.
    static const float CTCSS_TONES[] = {
        67.0f, 69.3f, 71.9f, 74.4f, 77.0f, 79.7f, 82.5f, 85.4f, 88.5f, 91.5f,
        94.8f, 97.4f, 100.0f, 103.5f, 107.2f, 110.9f, 114.8f, 118.8f, 123.0f, 127.3f,
        131.8f, 136.5f, 141.3f, 146.2f, 150.0f, 151.4f, 156.7f, 159.8f, 162.2f, 165.5f,
        167.9f, 171.3f, 173.8f, 177.3f, 179.9f, 183.5f, 186.2f, 189.9f, 192.8f, 196.6f,
        199.5f, 203.5f, 206.5f, 210.7f, 218.1f, 221.3f, 225.7f, 229.1f, 233.6f, 241.8f,
        250.3f, 254.1f
    };
    static const int CTCSS_TONE_COUNT = (int)(sizeof(CTCSS_TONES) / sizeof(CTCSS_TONES[0]));

    // What the squelch is listening for. A DCS code and its inverted form are
    // different waveforms - D023I is the same thing on air as D047N - so the
    // polarity has to be part of the selection, exactly as it is on a radio.
    struct Target {
        enum Mode {
            OFF,
            CTCSS,
            DCS
        };

        Mode mode = OFF;
        float ctcssFreq = 100.0f;
        int dcsCode = 23;
        bool dcsInverted = false;

        bool matches(const Result& r) const {
            switch (mode) {
            case CTCSS:
                return r.kind == Result::CTCSS && std::fabs(r.ctcssFreq - ctcssFreq) < 0.05f;
            case DCS:
                if (r.kind != Result::DCS) { return false; }
                return dcsInverted ? (r.dcsInverted == dcsCode) : (r.dcsNormal == dcsCode);
            default:
                return false;
            }
        }
    };

    static const int DCS_CODES_COUNT = 42;
    static const int DCS_POLARITIES = 2;
    static const int DCS_WORDS_PER_CODE = 5;

    // Each row is one code in both polarities; each entry lists the Golay words that
    // decode to it. Column 0 and column 1 of a row are the same transmission read
    // with opposite polarity.
    static const int DCS_CODES[DCS_CODES_COUNT][DCS_POLARITIES][DCS_WORDS_PER_CODE] = {
        { { 023, 0340, 0766, 0, 0 }, { 047, 0375, 0707, 0, 0 } },
        { { 025, 0, 0, 0, 0 }, { 0244, 0417, 0176, 0, 0 } },
        { { 026, 0566, 0, 0, 0 }, { 0464, 0642, 0772, 0237, 0 } },
        { { 031, 0374, 0643, 0, 0 }, { 0627, 037, 0560, 0, 0 } },
        { { 032, 0, 0, 0, 0 }, { 051, 0520, 0771, 0, 0 } },
        { { 043, 0355, 0, 0, 0 }, { 0445, 0457, 0575, 0222, 0 } },
        { { 054, 0405, 0675, 0, 0 }, { 0413, 0620, 0133, 0, 0 } },
        { { 065, 0301, 0, 0, 0 }, { 0271, 0427, 0510, 0762, 0 } },
        { { 071, 0603, 0717, 0746, 0 }, { 0306, 0761, 0147, 0303, 0 } },
        { { 072, 0470, 0701, 0, 0 }, { 0245, 0370, 0554, 0, 0 } },
        { { 073, 0640, 0, 0, 0 }, { 0506, 0574, 0224, 0313, 0 } },
        { { 074, 0360, 0721, 0, 0 }, { 0174, 0270, 0142, 0, 0 } },
        { { 0114, 0327, 0615, 0, 0 }, { 0712, 0136, 0502, 0, 0 } },
        { { 0115, 0534, 0674, 0, 0 }, { 0152, 0366, 0415, 0, 0 } },
        { { 0125, 0173, 0, 0, 0 }, { 0365, 0107, 0, 0, 0 } },
        { { 0131, 0572, 0702, 0, 0 }, { 0364, 0641, 0130, 0, 0 } },
        { { 0132, 0605, 0634, 0714, 0 }, { 0546, 0614, 0751, 0317, 0 } },
        { { 0134, 0273, 0, 0, 0 }, { 0223, 0350, 0475, 0750, 0 } },
        { { 0143, 0333, 0, 0, 0 }, { 0412, 0441, 0711, 0127, 0 } },
        { { 0155, 0233, 0660, 0, 0 }, { 0731, 0744, 0447, 0473, 0474 } },
        { { 0156, 0517, 0741, 0, 0 }, { 0265, 0426, 0171, 0, 0 } },
        { { 0162, 0416, 0553, 0, 0 }, { 0503, 0157, 0322, 0, 0 } },
        { { 0165, 0354, 0, 0, 0 }, { 0251, 0704, 0742, 0236, 0 } },
        { { 0172, 057, 0, 0, 0 }, { 036, 0137, 0, 0, 0 } },
        { { 0205, 0610, 0135, 0, 0 }, { 0263, 0736, 0213, 0, 0 } },
        { { 0226, 0557, 0104, 0, 0 }, { 0411, 0756, 0117, 0, 0 } },
        { { 0243, 0267, 0342, 0, 0 }, { 0351, 0353, 0435, 0, 0 } },
        { { 0261, 0567, 0227, 0, 0 }, { 0732, 0164, 0207, 0, 0 } },
        { { 0311, 0330, 0456, 0561, 0 }, { 0664, 0715, 0344, 0471, 0 } },
        { { 0315, 0321, 0673, 0, 0 }, { 0423, 0563, 0621, 0713, 0234 } },
        { { 0331, 0372, 0507, 0, 0 }, { 0465, 0656, 056, 0, 0 } },
        { { 0343, 0570, 0324, 0, 0 }, { 0532, 0161, 0345, 0, 0 } },
        { { 0346, 0616, 0635, 0724, 0 }, { 0612, 0706, 0254, 0314, 0 } },
        { { 0371, 0453, 0530, 0217, 0 }, { 0734, 066, 0, 0, 0 } },
        { { 0431, 0730, 0262, 0316, 0 }, { 0723, 0235, 0611, 0671, 0 } },
        { { 0432, 0276, 0326, 0, 0 }, { 0516, 0720, 067, 0, 0 } },
        { { 0466, 0666, 0144, 0, 0 }, { 0662, 0363, 0436, 0443, 0444 } },
        { { 0565, 0307, 0362, 0, 0 }, { 0703, 0150, 0256, 0, 0 } },
        { { 0606, 0630, 0153, 0, 0 }, { 0631, 0636, 0745, 0231, 0504 } },
        { { 0624, 075, 0501, 0, 0 }, { 0632, 0657, 0123, 0, 0 } },
        { { 0654, 0163, 0460, 0607, 0 }, { 0743, 0312, 0515, 0663, 0 } },
        { { 0754, 076, 0203, 0, 0 }, { 0116, 0734, 0, 0, 0 } }
    };

    static inline int octalToDecimal(int v) {
        return (v & 07) + ((v >> 3) & 07) * 10 + ((v >> 6) & 07) * 100;
    }

    // Given the nine code bits of a matched word, reports the code the transmitter
    // would be set to in each polarity.
    //
    // The column the word was found in is the code itself: a radio set to D023 sends
    // a word whose rotations are exactly column 0 of that row, and the complement of
    // that same waveform gives column 1. The squelch this table came from reads the
    // columns the other way round, which has it opening on the wrong polarity.
    static inline void golayFind(int v, int* normal, int* inverted) {
        *normal = 0;
        *inverted = 0;
        if (v == 0) { return; }
        for (int m = 0; m < DCS_CODES_COUNT; m++) {
            for (int n = 0; n < DCS_POLARITIES; n++) {
                for (int p = 0; p < DCS_WORDS_PER_CODE; p++) {
                    if (DCS_CODES[m][n][p] == 0 || DCS_CODES[m][n][p] != v) { continue; }
                    *normal = octalToDecimal(DCS_CODES[m][n][0]);
                    *inverted = octalToDecimal(DCS_CODES[m][(n + 1) % 2][0]);
                    return;
                }
            }
        }
    }

    // Checks the three sync bits and all eleven Golay parity bits of a 23 bit word.
    // Only a real DCS word at the right bit alignment passes, which is what lets the
    // decoder below find its alignment by sliding rather than by searching.
    static inline bool golayMatch(uint32_t v) {
        if ((v & (1u << 9)) != 0 || (v & (1u << 10)) != 0 || (v & (1u << 11)) == 0) { return false; }

        int c1 = (v >> 0) & 1, c2 = (v >> 1) & 1, c3 = (v >> 2) & 1;
        int c4 = (v >> 3) & 1, c5 = (v >> 4) & 1, c6 = (v >> 5) & 1;
        int c7 = (v >> 6) & 1, c8 = (v >> 7) & 1, c9 = (v >> 8) & 1;

        if ((int)((v >> 12) & 1) != ((c1 + c2 + c3 + c4 + c5 + c8) % 2)) { return false; }
        if ((int)((v >> 13) & 1) != (~((c2 + c3 + c4 + c5 + c6 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 14) & 1) != ((c1 + c2 + c6 + c7 + c8) % 2)) { return false; }
        if ((int)((v >> 15) & 1) != (~((c2 + c3 + c7 + c8 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 16) & 1) != (~((c1 + c2 + c5 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 17) & 1) != (~((c1 + c4 + c5 + c6 + c8) % 2) & 1)) { return false; }
        if ((int)((v >> 18) & 1) != ((c1 + c3 + c4 + c6 + c7 + c8 + c9) % 2)) { return false; }
        if ((int)((v >> 19) & 1) != ((c2 + c4 + c5 + c7 + c8 + c9) % 2)) { return false; }
        if ((int)((v >> 20) & 1) != ((c3 + c5 + c6 + c8 + c9) % 2)) { return false; }
        if ((int)((v >> 21) & 1) != (~((c4 + c6 + c7 + c9) % 2) & 1)) { return false; }
        if ((int)((v >> 22) & 1) != (~((c1 + c2 + c3 + c4 + c7) % 2) & 1)) { return false; }

        return true;
    }

    // Every code a transmitter can be set to, ascending, for the squelch's picker.
    // Both columns of the table qualify: a row's two columns are two different
    // codes, each of which some radio out there is set to.
    static inline std::vector<int> dcsCodeList() {
        std::vector<int> codes;
        codes.reserve(DCS_CODES_COUNT * DCS_POLARITIES);
        for (int m = 0; m < DCS_CODES_COUNT; m++) {
            for (int n = 0; n < DCS_POLARITIES; n++) {
                codes.push_back(octalToDecimal(DCS_CODES[m][n][0]));
            }
        }
        std::sort(codes.begin(), codes.end());
        codes.erase(std::unique(codes.begin(), codes.end()), codes.end());
        return codes;
    }

    // Sits in the audio chain rather than tapping off it, because a squelch has to
    // gate the samples on their way through, and the detection that decides the gate
    // has to look at the audio before it is muted - a tap after the gate would hear
    // silence and could never re-open.
    class ToneDetector : public dsp::Processor<dsp::stereo_t, dsp::stereo_t> {
        using base_type = dsp::Processor<dsp::stereo_t, dsp::stereo_t>;

        static const int CTCSS_WINDOW = 672;   // one second at the work rate
        static const int CTCSS_HOP = 336;      // analyse twice a second
        static const int FFT_SIZE = 8192;      // zero padded, for peak interpolation
        static const int PEAK_GUARD_BINS = 48; // the peak's own skirt, kept out of the floor
        static const int DCS_SPS = 5;
        static const int CONFIRMATIONS = 2;
        static const int HOLD_TICKS = 6;       // ~3 s of analyses, for the readout
        static const int GATE_HOLD_TICKS = 2;  // ~1 s squelch tail

    public:
        ToneDetector() {}

        ~ToneDetector() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            fftwf_destroy_plan(fftPlan);
            fftwf_free(fftIn);
            fftwf_free(fftOut);
        }

        void init(dsp::stream<dsp::stereo_t>* in, double inputSampleRate) {
            _inputSampleRate = inputSampleRate;
            // The resampler is driven from run() rather than run as a block of its
            // own, the way the audio waterfall drives its own, so it is initialised
            // with a null input.
            resamp.init(NULL, inputSampleRate, WORK_RATE);

            fftIn = (float*)fftwf_malloc(sizeof(float) * FFT_SIZE);
            fftOut = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * (FFT_SIZE / 2 + 1));
            memset(fftIn, 0, sizeof(float) * FFT_SIZE);
            fftPlan = fftwf_plan_dft_r2c_1d(FFT_SIZE, fftIn, fftOut, FFTW_ESTIMATE);

            // Hann, so a tone 2.4 Hz off its neighbour is not buried in that
            // neighbour's sidelobes.
            hann.resize(CTCSS_WINDOW);
            for (int i = 0; i < CTCSS_WINDOW; i++) {
                hann[i] = 0.5f - 0.5f * cosf(TWO_PI * (float)i / (float)(CTCSS_WINDOW - 1));
            }

            window.assign(CTCSS_WINDOW, 0.0f);
            spectrum.resize(FFT_SIZE / 2 + 1);
            reset();

            base_type::init(in);
        }

        void setInputSampleRate(double inputSampleRate) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _inputSampleRate = inputSampleRate;
            resamp.setInSamplerate(inputSampleRate);
            reset();
            base_type::tempStart();
        }

        // Drops everything learned so far, so a reading never outlives what produced
        // it. Called when the sample rate moves under it and when the demodulator
        // changes - which can happen with the block running, hence the stop: most of
        // the state below is touched by the worker without the result lock.
        void reset() {
            if (!base_type::_block_init) {
                resetState();
                return;
            }
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            resetState();
            base_type::tempStart();
        }

    private:
        void resetState() {
            std::lock_guard<std::mutex> lck(resultMtx);
            std::fill(window.begin(), window.end(), 0.0f);
            windowPos = 0;
            windowFilled = 0;
            sinceLastAnalysis = 0;
            dcBlockState = 0.0f;
            sampleIndex = 0;
            memset(signRing, 0, sizeof(signRing));
            memset(dcsShiftReg, 0, sizeof(dcsShiftReg));
            memset(dcsBitCount, 0, sizeof(dcsBitCount));
            ctcssCandidate = Result();
            ctcssCount = 0;
            dcsCandidate = Result();
            dcsCount = 0;
            dcsSeenThisTick = false;
            holdCounter = 0;
            published = Result();
            // Closed until something is heard, never open-by-default: a squelch that
            // fails open is not a squelch.
            gateHold = 0;
        }

    public:
        Result getResult() {
            std::lock_guard<std::mutex> lck(resultMtx);
            return published;
        }

        // Whether audio is currently getting through, for the UI to show. With the
        // squelch off this is always true.
        bool isGateOpen() {
            std::lock_guard<std::mutex> lck(resultMtx);
            return !squelchEnabled || gateHold > 0;
        }

        void setSquelch(bool enabled, const Target& target) {
            std::lock_guard<std::mutex> lck(resultMtx);
            bool wasEnabled = squelchEnabled;
            squelchEnabled = enabled && target.mode != Target::OFF;
            _target = target;
            // Start closed on every change, so switching to a different tone cannot
            // leave audio passing on the strength of the previous one's evidence.
            if (!wasEnabled || !squelchEnabled) { gateHold = 0; }
        }

        inline int process(int count, const dsp::stereo_t* in, dsp::stereo_t* out) {
            if ((int)monoBuf.size() < count) { monoBuf.resize(count); }
            for (int i = 0; i < count; i++) {
                monoBuf[i] = in[i].l;
            }

            // Always a downsample - audio never runs below 672 S/s - so the input
            // count bounds the output, with slack for the resampler's phase.
            if ((int)workBuf.size() < count + 64) { workBuf.resize(count + 64); }
            int workCount = resamp.process(count, monoBuf.data(), workBuf.data());

            for (int i = 0; i < workCount; i++) {
                // Single pole DC block. DCS is NRZ so its own average is near zero
                // over a word, but the demodulator's offset is not, and slicing
                // around a non-zero level turns every bit into a one.
                dcBlockState += DC_ALPHA * (workBuf[i] - dcBlockState);
                float s = workBuf[i] - dcBlockState;

                feedDCS(s);
                feedCTCSS(s);
            }

            bool pass;
            {
                std::lock_guard<std::mutex> lck(resultMtx);
                pass = !squelchEnabled || gateHold > 0;
            }
            if (pass) {
                memcpy(out, in, count * sizeof(dsp::stereo_t));
            }
            else {
                memset(out, 0, count * sizeof(dsp::stereo_t));
            }
            return count;
        }

        DEFAULT_PROC_RUN

    private:
        void feedCTCSS(float s) {
            window[windowPos] = s;
            windowPos = (windowPos + 1) % CTCSS_WINDOW;
            if (windowFilled < CTCSS_WINDOW) { windowFilled++; }

            if (++sinceLastAnalysis < CTCSS_HOP) { return; }
            sinceLastAnalysis = 0;
            if (windowFilled < CTCSS_WINDOW) { return; }

            tick(analyse());
        }

        // Returns the tone this window shows, or a NONE result if there isn't one.
        Result analyse() {
            // Unwrap the ring into the zero padded FFT input. The tail stays zero
            // from init, which is what buys the finer peak interpolation.
            for (int i = 0; i < CTCSS_WINDOW; i++) {
                fftIn[i] = window[(windowPos + i) % CTCSS_WINDOW] * hann[i];
            }
            fftwf_execute(fftPlan);

            const int bins = FFT_SIZE / 2 + 1;
            volk_32fc_s32f_power_spectrum_32f(spectrum.data(), (const lv_32fc_t*)fftOut, (float)FFT_SIZE, bins);

            int lo = binOf(SEARCH_LOW_HZ);
            int hi = binOf(SEARCH_HIGH_HZ);
            if (lo < 1) { lo = 1; }
            if (hi > bins - 2) { hi = bins - 2; }
            if (hi <= lo) { return Result(); }

            int peak = lo;
            for (int i = lo; i <= hi; i++) {
                if (spectrum[i] > spectrum[peak]) { peak = i; }
            }
            if (!std::isfinite(spectrum[peak])) { return Result(); }

            // Median of the band with the peak's own skirt masked out. A median
            // rather than a mean, so that a second tone or the bottom of speech
            // leaking in cannot drag the floor up and hide a real detection.
            floorBuf.clear();
            for (int i = lo; i <= hi; i++) {
                if (std::abs(i - peak) <= PEAK_GUARD_BINS) { continue; }
                if (!std::isfinite(spectrum[i])) { continue; }
                floorBuf.push_back(spectrum[i]);
            }
            if (floorBuf.size() < 16) { return Result(); }
            std::nth_element(floorBuf.begin(), floorBuf.begin() + floorBuf.size() / 2, floorBuf.end());
            float noiseFloor = floorBuf[floorBuf.size() / 2];

            if (spectrum[peak] - noiseFloor < CTCSS_MIN_SNR_DB) { return Result(); }

            // Parabolic interpolation over the dB peak. A one second window is far
            // too coarse on its own to tell 150.0 from 151.4 - the interpolated
            // position is what separates them.
            float y1 = spectrum[peak - 1];
            float y2 = spectrum[peak];
            float y3 = spectrum[peak + 1];
            float denom = y1 - 2.0f * y2 + y3;
            float delta = (denom != 0.0f) ? (0.5f * (y1 - y3) / denom) : 0.0f;
            if (!(delta > -1.0f && delta < 1.0f)) { delta = 0.0f; }
            float freq = ((float)peak + delta) * (float)WORK_RATE / (float)FFT_SIZE;

            int best = -1;
            float bestErr = SNAP_TOLERANCE_HZ;
            for (int i = 0; i < CTCSS_TONE_COUNT; i++) {
                float err = std::fabs(freq - CTCSS_TONES[i]);
                if (err < bestErr) {
                    bestErr = err;
                    best = i;
                }
            }
            if (best < 0) { return Result(); }

            Result r;
            r.kind = Result::CTCSS;
            r.ctcssFreq = CTCSS_TONES[best];
            return r;
        }

        void feedDCS(float s) {
            signRing[sampleIndex % DCS_SPS] = (s > 0.0f) ? 1 : 0;
            sampleIndex++;
            if (sampleIndex < DCS_SPS) { return; }

            int sum = 0;
            for (int i = 0; i < DCS_SPS; i++) { sum += signRing[i]; }
            int bit = (sum > DCS_SPS / 2) ? 1 : 0;

            // The bit phase is unknown, so five decoders run a fifth of a bit apart
            // and whichever is aligned locks. Each slides its word along one bit at a
            // time, so every rotation of the repeating code gets tested without the
            // brute force rotation loop the squelch decoder uses.
            int phase = (int)((sampleIndex - 1) % DCS_SPS);
            dcsShiftReg[phase] = ((dcsShiftReg[phase] >> 1) | ((uint32_t)bit << 22)) & 0x7FFFFF;
            if (dcsBitCount[phase] < 23) {
                dcsBitCount[phase]++;
                return;
            }

            uint32_t word = dcsShiftReg[phase];
            int normal = 0, inverted = 0;
            if (golayMatch(word)) {
                golayFind(word & 0777, &normal, &inverted);
            }
            else if (golayMatch(~word & 0x7FFFFF)) {
                golayFind((~word) & 0777, &inverted, &normal);
            }
            else {
                return;
            }
            if (normal == 0 && inverted == 0) { return; }

            Result r;
            r.kind = Result::DCS;
            r.dcsNormal = normal;
            r.dcsInverted = inverted;

            std::lock_guard<std::mutex> lck(resultMtx);
            if (r == dcsCandidate) {
                if (dcsCount < CONFIRMATIONS) { dcsCount++; }
            }
            else {
                dcsCandidate = r;
                dcsCount = 1;
            }
            dcsSeenThisTick = true;
        }

        // The half second CTCSS analysis is also the clock for publishing and for
        // ageing a reading out. A reading goes up only once two analyses agree and is
        // held for a few seconds after it stops arriving; without both, the badge
        // flickers on every noisy window and is unreadable.
        //
        // DCS wins when both are present. A channel carries one or the other, and a
        // locked Golay word is far harder to fake than a spectral peak.
        void tick(const Result& ctcss) {
            std::lock_guard<std::mutex> lck(resultMtx);

            if (ctcss.kind == Result::NONE) {
                ctcssCount = 0;
                ctcssCandidate = Result();
            }
            else if (ctcss == ctcssCandidate) {
                if (ctcssCount < CONFIRMATIONS) { ctcssCount++; }
            }
            else {
                ctcssCandidate = ctcss;
                ctcssCount = 1;
            }

            if (dcsSeenThisTick && dcsCount >= CONFIRMATIONS) {
                published = dcsCandidate;
                holdCounter = HOLD_TICKS;
            }
            else if (ctcssCount >= CONFIRMATIONS) {
                published = ctcssCandidate;
                holdCounter = HOLD_TICKS;
            }
            else if (holdCounter > 0) {
                holdCounter--;
                if (holdCounter == 0) { published = Result(); }
            }

            // The gate runs off the candidates rather than the published reading, so
            // it needs one agreeing analysis instead of two - about a second to open
            // instead of two. That is loose for identification, where a wrong answer
            // is shown to the user, but safe here: the frequency still has to snap to
            // the one tone that was asked for, or the Golay word still has to carry
            // the one code that was asked for.
            //
            // No fast path for opening. A quarter second window cannot separate 150.0
            // from 151.4 - they are inside each other's mainlobe at that length - so a
            // quick-opening squelch would let the neighbouring tone through. Slower
            // and right beats faster and wrong; the tail is short so speech is not
            // chopped once it is open.
            bool matched = (dcsSeenThisTick && dcsCount >= 1 && _target.matches(dcsCandidate)) ||
                           (ctcssCount >= 1 && _target.matches(ctcssCandidate));
            if (matched) { gateHold = GATE_HOLD_TICKS; }
            else if (gateHold > 0) { gateHold--; }

            if (!dcsSeenThisTick) {
                dcsCandidate = Result();
                dcsCount = 0;
            }
            dcsSeenThisTick = false;
        }

        static int binOf(float hz) {
            return (int)(hz * (float)FFT_SIZE / (float)WORK_RATE);
        }

        static constexpr double WORK_RATE = 672.0; // 5 samples per DCS bit, > 2 x 254.1 Hz
        static constexpr float TWO_PI = 6.283185307179586f;
        static constexpr float SEARCH_LOW_HZ = 60.0f;
        static constexpr float SEARCH_HIGH_HZ = 260.0f;
        static constexpr float CTCSS_MIN_SNR_DB = 10.0f;
        static constexpr float SNAP_TOLERANCE_HZ = 0.6f; // < half the 1.4 Hz 150.0/151.4 gap
        static constexpr float DC_ALPHA = 0.002f;

        double _inputSampleRate = 48000.0;
        dsp::multirate::RationalResampler<float> resamp;

        std::vector<float> monoBuf;
        std::vector<float> workBuf;

        std::vector<float> window;
        std::vector<float> hann;
        std::vector<float> spectrum;
        std::vector<float> floorBuf;
        int windowPos = 0;
        int windowFilled = 0;
        int sinceLastAnalysis = 0;
        float dcBlockState = 0.0f;

        float* fftIn = NULL;
        fftwf_complex* fftOut = NULL;
        fftwf_plan fftPlan = NULL;

        long long sampleIndex = 0;
        int signRing[DCS_SPS] = { 0 };
        uint32_t dcsShiftReg[DCS_SPS] = { 0 };
        int dcsBitCount[DCS_SPS] = { 0 };

        std::mutex resultMtx;
        Result ctcssCandidate;
        int ctcssCount = 0;
        Result dcsCandidate;
        int dcsCount = 0;
        bool dcsSeenThisTick = false;
        int holdCounter = 0;
        Result published;

        bool squelchEnabled = false;
        Target _target;
        int gateHold = 0;
    };
}
