#pragma once
#include "tone_tables.h"
#include <dsp/processor.h>
#include <dsp/types.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/filter/fir.h>
#include <dsp/taps/high_pass.h>
#include <dsp/taps/tap.h>
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
        static const int GATE_ARM_TICKS = 2;   // how long an identification stays valid
        static const int FAST_WINDOW = 112;    // 1/6 s - the squelch's closing latency

    public:
        ToneDetector() {}

        ~ToneDetector() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            fftwf_destroy_plan(fftPlan);
            fftwf_free(fftIn);
            fftwf_free(fftOut);
            dsp::taps::free(filterTaps);
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

            // Its own, much shorter Hann for the fast presence check
            fastHann.resize(FAST_WINDOW);
            for (int i = 0; i < FAST_WINDOW; i++) {
                fastHann[i] = 0.5f - 0.5f * cosf(TWO_PI * (float)i / (float)(FAST_WINDOW - 1));
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
            // The taps are cut for a specific rate, so they have to be recut
            if (filterEnabled) { buildFilter(); }
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
        // 300 Hz corner with a 100 Hz transition, the same numbers the FM
        // demodulator's own high pass uses: it puts the stopband edge just below
        // 254.1, the highest CTCSS tone, while leaving speech alone.
        void buildFilter() {
            dsp::tap<float> newTaps = dsp::taps::highPass(FILTER_CUTOFF_HZ, FILTER_TRANS_HZ, _inputSampleRate);
            dsp::tap<float> oldTaps = filterTaps;
            if (!filterReady) {
                toneFilter.init(NULL, newTaps);
                filterReady = true;
            }
            else {
                toneFilter.setTaps(newTaps);
            }
            filterTaps = newTaps;
            dsp::taps::free(oldTaps);
        }

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
            gateArmed = 0;
            fastPresent = false;
            fastPos = 0;
            fastTunedTo = 0.0f;
            lastDcsMatchSample = -DCS_PRESENCE_SAMPLES;
            for (int k = 0; k < 3; k++) { fastS1[k] = 0.0f; fastS2[k] = 0.0f; }
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
            return !squelchEnabled || (gateArmed > 0 && fastPresent);
        }

        // Strips the tone out of what reaches the speaker, the way a radio does it.
        //
        // It has to live here rather than in the demodulator, which already has a
        // high pass with these exact settings: that one runs before the audio chain,
        // so switching it on removes the tone before this block ever sees it and
        // detection stops working. A radio wires the discriminator to the tone
        // decoder and, separately, through a high pass to the audio. So does this -
        // the filter is applied on the way out, and detection keeps reading the
        // untouched input.
        //
        // The filter is built on first use. It is a windowed sinc of a few thousand
        // taps at audio rate, so it is not cheap and not something to allocate for
        // people who leave it off.
        void setToneFilter(bool enable) {
            if (!base_type::_block_init) {
                filterEnabled = enable;
                return;
            }
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            filterEnabled = enable;
            if (filterEnabled) { buildFilter(); }
            base_type::tempStart();
        }

        void setSquelch(bool enabled, const Target& target) {
            std::lock_guard<std::mutex> lck(resultMtx);
            bool wasEnabled = squelchEnabled;
            squelchEnabled = enabled && target.mode != Target::OFF;
            _target = target;
            // Start closed on every change, so switching to a different tone cannot
            // leave audio passing on the strength of the previous one's evidence.
            if (!wasEnabled || !squelchEnabled) { gateArmed = 0; }
            fastPresent = false;
            fastTunedTo = 0.0f;
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
                feedFast(s);
                feedCTCSS(s);
            }

            // Filter first, gate second. The convolution has to keep running while
            // the gate is shut or its history would be full of silence and the first
            // moment after it opens would be a thump.
            if (filterEnabled && filterReady) {
                if ((int)filteredBuf.size() < count) { filteredBuf.resize(count); }
                toneFilter.process(count, monoBuf.data(), filteredBuf.data());
                for (int i = 0; i < count; i++) {
                    out[i].l = filteredBuf[i];
                    out[i].r = filteredBuf[i];
                }
            }
            else {
                memcpy(out, in, count * sizeof(dsp::stereo_t));
            }

            bool pass;
            {
                std::lock_guard<std::mutex> lck(resultMtx);
                pass = !squelchEnabled || (gateArmed > 0 && fastPresent);
            }
            if (!pass) {
                memset(out, 0, count * sizeof(dsp::stereo_t));
            }
            return count;
        }

        DEFAULT_PROC_RUN

    private:
        // Opening and closing are not the same problem, and using one mechanism for
        // both is what made the squelch hang open for up to a second and a half after
        // a transmission - a carrier drop turns into full scale hiss, so that is a
        // second and a half of noise at whatever volume was set for a weak signal.
        //
        // Opening has to decide which tone this is, which needs the long window: at a
        // quarter of a second 150.0 and 151.4 are inside each other's mainlobe.
        // Closing only has to answer whether the tone that was already identified is
        // still there, and that is one Goertzel at a frequency already known. So the
        // slow path still arms the gate and the fast path holds it open, which closes
        // it within a sixth of a second of the tone stopping.
        void feedFast(float s) {
            float w = fastHann[fastPos];
            for (int k = 0; k < 3; k++) {
                float s0 = (s * w) + fastCoeff[k] * fastS1[k] - fastS2[k];
                fastS2[k] = fastS1[k];
                fastS1[k] = s0;
            }
            if (++fastPos < FAST_WINDOW) { return; }
            fastPos = 0;
            evaluateFast();
            for (int k = 0; k < 3; k++) { fastS1[k] = 0.0f; fastS2[k] = 0.0f; }
        }

        // Runs six times a second, so reading the target under the lock here costs
        // nothing, unlike doing it per sample.
        void evaluateFast() {
            float power[3];
            for (int k = 0; k < 3; k++) {
                power[k] = (fastS1[k] * fastS1[k]) + (fastS2[k] * fastS2[k]) - (fastCoeff[k] * fastS1[k] * fastS2[k]);
            }

            std::lock_guard<std::mutex> lck(resultMtx);

            // The references sit far enough off the tone to be outside the mainlobe
            // of a Hann window this short, so they read the noise either side of it.
            // Comparing against them rather than against the block's total energy is
            // what stops speech - which is far louder than the tone - from reading as
            // the tone having gone.
            if (_target.mode == Target::DCS) {
                fastPresent = (sampleIndex - lastDcsMatchSample) < DCS_PRESENCE_SAMPLES;
            }
            else if (_target.mode == Target::CTCSS) {
                if (_target.ctcssFreq != fastTunedTo) { retuneFast(_target.ctcssFreq); }
                float reference = std::max(power[1], power[2]);
                fastPresent = power[0] > FAST_MIN_RATIO * reference;
            }
            else {
                fastPresent = false;
            }
        }

        void retuneFast(float freq) {
            fastTunedTo = freq;
            const float f[3] = { freq, freq - FAST_REF_OFFSET_HZ, freq + FAST_REF_OFFSET_HZ };
            for (int k = 0; k < 3; k++) {
                fastCoeff[k] = 2.0f * cosf(TWO_PI * f[k] / (float)WORK_RATE);
                fastS1[k] = 0.0f;
                fastS2[k] = 0.0f;
            }
        }

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
            // Only a match on the code actually being listened for keeps the gate
            // open, hence the target check rather than just "some code decoded".
            if (_target.matches(r)) { lastDcsMatchSample = sampleIndex; }
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
            // Arms the gate; the fast presence check is what actually holds it
            // open, so this only has to remember that the right tone was identified
            // recently enough for a momentary dropout not to need re-deciding.
            if (matched) { gateArmed = GATE_ARM_TICKS; }
            else if (gateArmed > 0) { gateArmed--; }

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
        // References either side of the tone, far enough out to clear the mainlobe of
        // a Hann window FAST_WINDOW long, so they measure the noise beside the tone
        // rather than the tone itself.
        static constexpr float FAST_REF_OFFSET_HZ = 20.0f;
        static constexpr float FAST_MIN_RATIO = 4.0f; // 6 dB over the noise beside it
        // A locked DCS decoder produces a match every bit, so a quarter second
        // without one means it has stopped.
        static const int DCS_PRESENCE_SAMPLES = 168;
        static constexpr double FILTER_CUTOFF_HZ = 300.0;
        static constexpr double FILTER_TRANS_HZ = 100.0;

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
        int gateArmed = 0;

        // Fast "is that tone still there" monitor, for closing
        std::vector<float> fastHann;
        float fastCoeff[3] = { 0.0f, 0.0f, 0.0f };
        float fastS1[3] = { 0.0f, 0.0f, 0.0f };
        float fastS2[3] = { 0.0f, 0.0f, 0.0f };
        int fastPos = 0;
        float fastTunedTo = 0.0f;
        bool fastPresent = false;
        long long lastDcsMatchSample = 0;

        // The mono filter rather than FIR<stereo_t, float>, because NFM audio is mono
        // duplicated across both channels: filtering once and copying is half the
        // convolution for the same result. The stereo form is correct now, it is just
        // twice the work here.
        dsp::filter::FIR<float, float> toneFilter;
        std::vector<float> filteredBuf;
        dsp::tap<float> filterTaps;
        bool filterEnabled = false;
        bool filterReady = false;
    };
}
