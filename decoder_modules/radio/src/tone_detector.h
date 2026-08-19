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
#include <chrono>
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


    // One RBJ notch, direct form 2 transposed. Used to take the harmonics of the
    // signalling tone out of the audio - see retuneNotches for why they are there.
    struct ToneNotch {
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
        float z1 = 0.0f, z2 = 0.0f;
        bool active = false;

        void set(double f0, double q, double fs) {
            double w0 = 2.0 * 3.14159265358979 * f0 / fs;
            double alpha = sin(w0) / (2.0 * q);
            double a0 = 1.0 + alpha;
            double c = -2.0 * cos(w0);
            b0 = (float)(1.0 / a0);
            b1 = (float)(c / a0);
            b2 = (float)(1.0 / a0);
            a1 = (float)(c / a0);
            a2 = (float)((1.0 - alpha) / a0);
            active = true;
        }

        void clear() {
            active = false;
            z1 = 0.0f;
            z2 = 0.0f;
        }

        inline float process(float x) {
            float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };

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
        static const int BURST_BLOCK = 21;     // ~31 ms, the phase detector's step
        static const int BURST_HISTORY = 4;    // ~125 ms of it, shorter than any burst
        static const int MAX_CANDIDATES = 3;   // tones reported per analysis
        static const int MAX_PEAKS = 5;        // peaks examined to find them

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

        // The high pass alone does not silence the tone, only its fundamental.
        //
        // A CTCSS encoder in a handheld is not a clean sine, and the harmonics of what
        // it sends land above the 300 Hz corner as soon as the tone itself is above
        // 150 Hz: 210.7 Hz puts its second harmonic at 421.4 Hz, where the high pass
        // does nothing at all, and what is left is plainly audible as a buzz. The low
        // tones are quiet because their harmonics fall inside the stopband too - which
        // is why this only shows up on the top half of the table.
        //
        // So notch out where those harmonics have to be. The frequency is known
        // exactly, which is what makes a notch this narrow safe: constant Q rather
        // than constant width, because a transmitter's frequency error is a percentage
        // and so grows with the harmonic it is being measured on.
        void retuneNotches(float fundamental) {
            notchTunedTo = fundamental;
            for (int n = 0; n < HARMONIC_NOTCHES; n++) {
                double f = (double)fundamental * (double)(n + 2);
                // Nothing to do for a tone whose harmonics the high pass already
                // covers, and nothing sensible to do near Nyquist.
                if (fundamental <= 0.0f || f < FILTER_CUTOFF_HZ || f > 0.45 * _inputSampleRate) {
                    harmonicNotch[n].clear();
                    continue;
                }
                harmonicNotch[n].set(f, NOTCH_Q, _inputSampleRate);
            }
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
            openKey = ToneKey();
            openKeyValid = false;
            fastPresent = false;
            fastPos = 0;
            fastTunedTo = 0.0f;
            tailLockout = 0;
            burstTunedTo = 0.0f;
            burstStep = 0.0f;
            resetBurst();
            lastOpenKeyValid = false;
            notchTunedTo = 0.0f;
            for (int n = 0; n < HARMONIC_NOTCHES; n++) { harmonicNotch[n].clear(); }
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

        // Which code the gate has latched onto, for the UI to name. In the modes that
        // accept more than one code this is the only way to say which of them opened
        // it; in the single-code modes it is the one that was asked for. False while
        // nothing is latched.
        bool getOpenKey(ToneKey* out) {
            std::lock_guard<std::mutex> lck(resultMtx);
            if (!openKeyValid) { return false; }
            *out = openKey;
            return true;
        }

        // The code that most recently held the gate open, kept for a couple of seconds
        // after it shuts. getOpenKey answers for the present moment and goes blank the
        // instant the gate closes, which on a short transmission - and with the tail
        // detector shortening them further - is before anyone has looked up at it.
        bool getLastOpenKey(ToneKey* out) {
            std::lock_guard<std::mutex> lck(resultMtx);
            if (!lastOpenKeyValid) { return false; }
            auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - lastOpenAt).count();
            if (age > OPEN_KEY_HOLD_MS) { return false; }
            *out = lastOpenKey;
            return true;
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

        // Whether to act on the burst a radio sends to say it has finished, rather
        // than waiting for the tone itself to stop. Cheap either way - the phase
        // detector only runs while the gate is open on a CTCSS tone - so this is a
        // switch for people whose radios do not send one, or whose channel is noisy
        // enough that they would rather have the tail than risk a clipped word.
        void setTailClose(bool enable) {
            std::lock_guard<std::mutex> lck(resultMtx);
            tailCloseEnabled = enable;
            tailLockout = 0;
        }

        void setSquelch(bool enabled, const Target& target) {
            std::lock_guard<std::mutex> lck(resultMtx);
            squelchEnabled = enabled && target.mode != Target::OFF;
            _target = target;
            // Start closed on every change, so switching to a different tone cannot
            // leave audio passing on the strength of the previous one's evidence. It
            // has to be unconditional: clearing it only on an on/off transition left
            // a target change carrying up to GATE_ARM_TICKS of arming earned by the
            // code that was selected before.
            gateArmed = 0;
            openKeyValid = false;
            fastPresent = false;
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
                feedBurst(s);
                feedCTCSS(s);
            }

            // Filter first, gate second. The convolution has to keep running while
            // the gate is shut or its history would be full of silence and the first
            // moment after it opens would be a thump.
            if (filterEnabled && filterReady) {
                if ((int)filteredBuf.size() < count) { filteredBuf.resize(count); }
                toneFilter.process(count, monoBuf.data(), filteredBuf.data());
                // Retuned from whichever tone is currently known, once per buffer
                // rather than per sample - it can only change every half second.
                float wantNotch = 0.0f;
                {
                    std::lock_guard<std::mutex> lck(resultMtx);
                    // The measured frequency in preference to the standard one it
                    // snapped to. A transmitter is allowed to be 1% off, and 1% off is
                    // 4 Hz out on a second harmonic - which turns a notch this narrow
                    // from burying the harmonic into taking 11 dB off it. What the peak
                    // interpolation reads is good to about a twentieth of a Hz, so the
                    // notch can sit where the harmonic really is instead.
                    if (published.kind == Result::CTCSS) {
                        wantNotch = (published.measuredHz > 0.0f) ? published.measuredHz : published.ctcssFreq;
                    }
                    else if (openKeyValid && openKey.kind == ToneKey::CTCSS) {
                        wantNotch = openKey.ctcssFreq;
                    }
                }
                // Deadbanded: the reading moves a little every analysis, and rebuilding
                // three biquads on every buffer to chase hundredths of a Hz is work for
                // nothing.
                bool notchChanged = (wantNotch == 0.0f) != (notchTunedTo == 0.0f);
                if (!notchChanged && wantNotch != 0.0f) {
                    notchChanged = std::fabs(wantNotch - notchTunedTo) > NOTCH_RETUNE_HZ;
                }
                if (notchChanged) { retuneNotches(wantNotch); }
                for (int n = 0; n < HARMONIC_NOTCHES; n++) {
                    if (!harmonicNotch[n].active) { continue; }
                    for (int i = 0; i < count; i++) {
                        filteredBuf[i] = harmonicNotch[n].process(filteredBuf[i]);
                    }
                }
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
                if (pass && openKeyValid) {
                    lastOpenKey = openKey;
                    lastOpenKeyValid = true;
                    lastOpenAt = std::chrono::steady_clock::now();
                }
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

        // Runs six times a second, so reading the latched code under the lock here
        // costs nothing, unlike doing it per sample.
        //
        // It follows the code the gate actually latched onto rather than the target,
        // because in the modes that accept more than one code the target does not name
        // a frequency to watch - only the identification does.
        void evaluateFast() {
            float power[3];
            for (int k = 0; k < 3; k++) {
                power[k] = (fastS1[k] * fastS1[k]) + (fastS2[k] * fastS2[k]) - (fastCoeff[k] * fastS1[k] * fastS2[k]);
            }

            std::lock_guard<std::mutex> lck(resultMtx);

            if (!openKeyValid) {
                fastPresent = false;
            }
            else if (openKey.kind == ToneKey::DCS) {
                bool dataThere = (sampleIndex - lastDcsMatchSample) < DCS_PRESENCE_SAMPLES;
                // DCS says it has finished with a turn-off code: 134.4 Hz on its own
                // for about 180 ms, sent in place of the data. Only looked for once the
                // data has stopped arriving, because the data is itself a 134.4 bps
                // waveform and carries energy of its own at that frequency; and judged
                // against the same off-tone references the CTCSS check uses, so that
                // the hiss after a carrier drop - which fills all three equally - does
                // not read as a turn-off code.
                if (tailCloseEnabled && !dataThere && fastTunedTo == DCS_TURNOFF_HZ) {
                    float reference = (power[1] > power[2]) ? power[1] : power[2];
                    if (power[0] > FAST_MIN_RATIO * reference) {
                        closeOnTail();
                        return;
                    }
                }
                fastPresent = dataThere;
                // Nothing else uses the Goertzels while a DCS code holds the gate, so
                // they sit on the turn-off frequency for as long as it does.
                retuneFast(DCS_TURNOFF_HZ);
            }
            else {
                // This frame was accumulated with whatever the Goertzels were tuned to
                // while it ran, so it only answers for that frequency. Judge it first
                // and retune for the next frame afterwards: retuning first reads a
                // frame measured at one frequency as an answer about another, and on
                // the first frame after arming that means judging a Goertzel whose
                // coefficients were still zero - a tone at a quarter of the work rate,
                // which is nothing that was ever asked about.
                if (openKey.ctcssFreq == fastTunedTo) {
                    // The references sit far enough off the tone to be outside the
                    // mainlobe of a Hann window this short, so they read the noise
                    // either side of it. Comparing against them rather than against
                    // the block's total energy is what stops speech - which is far
                    // louder than the tone - from reading as the tone having gone.
                    //
                    // Not std::max: core compiles this header through
                    // mobile_main_window.cpp, which drags in windows.h, where max is a
                    // function-like macro and std::max(a, b) becomes std::(a, b).
                    float reference = (power[1] > power[2]) ? power[1] : power[2];
                    fastPresent = power[0] > FAST_MIN_RATIO * reference;
                }
                retuneFast(openKey.ctcssFreq);
            }
        }

        void retuneFast(float freq) {
            if (freq == fastTunedTo) { return; }
            fastTunedTo = freq;
            const float f[3] = { freq, freq - FAST_REF_OFFSET_HZ, freq + FAST_REF_OFFSET_HZ };
            for (int k = 0; k < 3; k++) {
                fastCoeff[k] = 2.0f * cosf(TWO_PI * f[k] / (float)WORK_RATE);
                fastS1[k] = 0.0f;
                fastS2[k] = 0.0f;
            }
        }

        // Closes the moment the transmitter says it has finished, instead of waiting
        // for the tone to stop and letting a sixth of a second of hiss out first.
        //
        // A radio ending a transmission does not simply cut the tone. It flips the
        // tone's phase - 120 or 180 degrees depending on the make - and holds it there
        // for a couple of hundred milliseconds before dropping the carrier. Handhelds
        // watch for that flip and mute on it, and that is what makes the end of a
        // transmission silent instead of a burst of noise.
        //
        // Mixing the audio down by the tone's own frequency leaves a phase that only
        // creeps, at whatever the transmitter's frequency error happens to be, so the
        // creep can be measured and taken back out. What is left is flat until the
        // flip. Nothing else below 300 Hz produces a step that size - transmitters high
        // pass their microphone audio at 300 Hz precisely so this band stays clear for
        // signalling, which is what makes the phase readable at all.
        void feedBurst(float s) {
            burstI += s * cosf(burstPhase);
            burstQ -= s * sinf(burstPhase);
            burstPhase += burstStep;
            if (burstPhase > TWO_PI) { burstPhase -= TWO_PI; }
            if (++burstPos < BURST_BLOCK) { return; }
            burstPos = 0;
            evaluateBurst();
            burstI = 0.0f;
            burstQ = 0.0f;
        }

        void evaluateBurst() {
            std::lock_guard<std::mutex> lck(resultMtx);

            // Only ever run against the code the gate actually latched onto, and only
            // while the gate is what is keeping the audio on. DCS has an ending of its
            // own and is handled in the fast path.
            float want = 0.0f;
            if (tailCloseEnabled && squelchEnabled && openKeyValid && openKey.kind == ToneKey::CTCSS) {
                want = openKey.ctcssFreq;
            }
            if (want != burstTunedTo) {
                burstTunedTo = want;
                burstStep = TWO_PI * want / (float)WORK_RATE;
                resetBurst();
                return;
            }
            if (want == 0.0f) { return; }

            float mag = sqrtf(burstI * burstI + burstQ * burstQ);
            float phase = atan2f(burstQ, burstI);
            burstMag += BURST_MAG_ALPHA * (mag - burstMag);
            if (!burstPrevValid) {
                burstPrev = phase;
                burstPrevValid = true;
                return;
            }
            float step = wrapPi(phase - burstPrev);
            burstPrev = phase;

            // The phase of noise measures nothing, and the instant the carrier goes
            // that is all there is - so a block the tone has dropped out of is thrown
            // away rather than read.
            if (mag < BURST_MIN_MAG_RATIO * burstMag) {
                resetBurstHistory();
                return;
            }

            float resid = step - burstDrift;
            // A running mean while the estimate is still being formed, a slow EMA once
            // it is. Going straight to the EMA from zero takes it a couple of seconds
            // to catch up with a transmitter a few Hz off its nominal tone, and until
            // it has, the creep it has not accounted for yet looks like the start of a
            // flip - which at a poor signal to noise ratio is enough to close the gate
            // in the middle of a transmission. Once formed it is slow on purpose: far
            // too slow to follow a flip that is over in a fifth of a second.
            float alpha = (burstSettle < BURST_SETTLE_BLOCKS) ? (1.0f / (float)(burstSettle + 1)) : BURST_DRIFT_ALPHA;
            burstDrift += alpha * (step - burstDrift);
            burstHist[burstHistPos] = resid;
            burstHistPos = (burstHistPos + 1) % BURST_HISTORY;
            if (burstSettle < BURST_SETTLE_BLOCKS) {
                burstSettle++;
                return;
            }

            // Summed over the history rather than taken a block at a time, so that a
            // flip landing across a block boundary still counts for all of itself.
            float sum = 0.0f;
            for (int k = 0; k < BURST_HISTORY; k++) { sum += burstHist[k]; }
            int sign = (sum > BURST_PHASE_STEP) ? 1 : ((sum < -BURST_PHASE_STEP) ? -1 : 0);
            // Two blocks running, leaning the same way both times. One window over the
            // line is what noise does now and again; a real flip holds the window over
            // it for as long as the window is, and always with the same sign. The cost
            // of the extra block is 31 ms of the couple of hundred the burst lasts, and
            // the cost of getting it wrong is a muted word.
            if (sign != 0 && sign == burstOverSign) {
                closeOnTail();
                return;
            }
            burstOverSign = sign;
        }

        // Shuts the gate on an end-of-transmission burst, from whichever detector saw
        // it. Called with resultMtx held.
        void closeOnTail() {
            gateArmed = 0;
            openKeyValid = false;
            fastPresent = false;
            // The tone is still on air while the burst is running, so the next analysis
            // would find it and open the gate straight back up - which is precisely the
            // tail this exists to remove. Counted in ticks rather than left to wait for
            // the tone to go, so that a false reading costs a second or two of audio
            // and not the rest of the transmission; and cleared early in tick() as soon
            // as nothing is being heard, which is what normally happens first.
            tailLockout = TAIL_LOCKOUT_TICKS;
            resetBurst();
            burstTunedTo = 0.0f;
            burstStep = 0.0f;
        }

        void resetBurstHistory() {
            for (int k = 0; k < BURST_HISTORY; k++) { burstHist[k] = 0.0f; }
            burstHistPos = 0;
            burstOverSign = 0;
            burstPrevValid = false;
            burstSettle = 0;
        }

        void resetBurst() {
            resetBurstHistory();
            burstI = 0.0f;
            burstQ = 0.0f;
            burstPos = 0;
            burstPhase = 0.0f;
            burstPrev = 0.0f;
            burstDrift = 0.0f;
            burstMag = 0.0f;
        }

        static float wrapPi(float a) {
            while (a > PI_F) { a -= TWO_PI; }
            while (a < -PI_F) { a += TWO_PI; }
            return a;
        }

        void feedCTCSS(float s) {
            window[windowPos] = s;
            windowPos = (windowPos + 1) % CTCSS_WINDOW;
            if (windowFilled < CTCSS_WINDOW) { windowFilled++; }

            if (++sinceLastAnalysis < CTCSS_HOP) { return; }
            sinceLastAnalysis = 0;
            if (windowFilled < CTCSS_WINDOW) { return; }

            Result candidates[MAX_CANDIDATES];
            int found = analyse(candidates);
            tick(candidates, found);
        }

        // Fills `out` with the tones this window shows, loudest first, and returns how
        // many there were - none if there is nothing above the noise that lands on a
        // standard tone.
        //
        // More than one, because the loudest thing between 60 and 260 Hz is not always
        // the signalling tone. Mains hum is both a legitimate CTCSS frequency at 100
        // and 120 Hz and the biggest peak in the band on plenty of receivers, and a
        // search that reports only the strongest peak hands the gate that instead of
        // the tone it was told to open for.
        int analyse(Result* out) {
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
            if (hi <= lo) { return 0; }

            // A quick bail on a spectrum that is not one. The peel below skips non
            // finite bins as it meets them; this catches a block that is garbage all
            // the way through without walking it several times over to find that out.
            int peak = lo;
            for (int i = lo; i <= hi; i++) {
                if (spectrum[i] > spectrum[peak]) { peak = i; }
            }
            if (!std::isfinite(spectrum[peak])) { return 0; }

            // Peel the peaks off in order of height, masking out the skirt of each one
            // taken so the next pass finds a separate tone rather than the side of the
            // one before it - the same guard width the noise floor uses, for the same
            // reason. Room for more peaks than tones reported, because a peak that
            // snaps to no standard tone still has to be taken off the table before the
            // next pass can see past it, and mains hum at 60 Hz - below the lowest
            // CTCSS tone at 67.0 - is exactly that case.
            int claimed[MAX_PEAKS];
            int claimedCount = 0;
            int count = 0;
            while (claimedCount < MAX_PEAKS && count < MAX_CANDIDATES) {
                int p = -1;
                for (int i = lo; i <= hi; i++) {
                    if (!std::isfinite(spectrum[i])) { continue; }
                    bool masked = false;
                    for (int c = 0; c < claimedCount; c++) {
                        if (std::abs(i - claimed[c]) <= PEAK_GUARD_BINS) {
                            masked = true;
                            break;
                        }
                    }
                    if (masked) { continue; }
                    if (p < 0 || spectrum[i] > spectrum[p]) { p = i; }
                }
                if (p < 0) { break; }
                float noiseFloor;
                if (!localFloor(p, lo, hi, &noiseFloor)) { break; }
                // Ordered by height, so the first peak too quiet to call is also the
                // last: nothing further down the list can clear the threshold.
                if (spectrum[p] - noiseFloor < CTCSS_MIN_SNR_DB) { break; }
                claimed[claimedCount++] = p;

                // Parabolic interpolation over the dB peak. A one second window is far
                // too coarse on its own to tell 150.0 from 151.4 - the interpolated
                // position is what separates them.
                float y1 = spectrum[p - 1];
                float y2 = spectrum[p];
                float y3 = spectrum[p + 1];
                float denom = y1 - 2.0f * y2 + y3;
                float delta = (denom != 0.0f) ? (0.5f * (y1 - y3) / denom) : 0.0f;
                if (!(delta > -1.0f && delta < 1.0f)) { delta = 0.0f; }
                float freq = ((float)p + delta) * (float)WORK_RATE / (float)FFT_SIZE;

                int best = -1;
                float bestErr = SNAP_TOLERANCE_HZ;
                for (int i = 0; i < CTCSS_TONE_COUNT; i++) {
                    float err = std::fabs(freq - CTCSS_TONES[i]);
                    if (err < bestErr) {
                        bestErr = err;
                        best = i;
                    }
                }
                // A peak that lands between two standard tones is something else on
                // the channel, not a tone. Skip it and keep looking - it is exactly
                // the case that used to hide the real one.
                if (best < 0) { continue; }

                out[count].kind = Result::CTCSS;
                out[count].ctcssFreq = CTCSS_TONES[best];
                out[count].measuredHz = freq;
                count++;
            }
            return count;
        }

        // The noise a peak has to stand out from, measured beside that peak rather
        // than across the whole search band.
        //
        // An FM discriminator's noise is not flat: its power rises as the square of
        // frequency, so across 60 to 260 Hz the floor climbs by nearly 13 dB. A median
        // taken over all of it therefore sits well below the real floor at the top of
        // the band and well above it at the bottom - which cost sensitivity on the low
        // tones and, far worse, handed the high ones several dB of free headroom. On a
        // dead channel that turned noise into identifications, and they were not spread
        // evenly: two thirds of them landed above 200 Hz, on 254.1, 250.3, 241.8 and
        // their neighbours, because those are where the bias was largest.
        //
        // Measured over a window either side of the peak the slope is small, so what
        // comes back is the floor the peak actually has to beat.
        //
        // Still a median rather than a mean, for the original reason: a second tone or
        // the bottom of speech leaking in must not drag the floor up and hide a real
        // detection.
        bool localFloor(int p, int lo, int hi, float* out) {
            int half = binOf(FLOOR_WINDOW_HZ);
            int a = p - half;
            int b = p + half;
            // Slid rather than clipped at the edges of the search band, so that a tone
            // at 67.0 or 254.1 is judged against as many bins as one in the middle.
            if (a < lo) {
                a = lo;
                b = (hi < lo + 2 * half) ? hi : (lo + 2 * half);
            }
            if (b > hi) {
                b = hi;
                a = (lo > hi - 2 * half) ? lo : (hi - 2 * half);
            }
            floorBuf.clear();
            for (int i = a; i <= b; i++) {
                if (std::abs(i - p) <= PEAK_GUARD_BINS) { continue; }
                if (!std::isfinite(spectrum[i])) { continue; }
                floorBuf.push_back(spectrum[i]);
            }
            if (floorBuf.size() < 16) { return false; }
            std::nth_element(floorBuf.begin(), floorBuf.begin() + floorBuf.size() / 2, floorBuf.end());
            *out = floorBuf[floorBuf.size() / 2];
            return true;
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
            // Once the gate has latched onto one code, that code alone holds it:
            // otherwise on a channel set to accept several, one user's code would hold
            // open a gate that another user's code opened, and the audio would never
            // close between them.
            if (openKeyValid ? openKey.matches(r) : _target.matches(r)) { lastDcsMatchSample = sampleIndex; }
        }

        // The half second CTCSS analysis is also the clock for publishing and for
        // ageing a reading out. A reading goes up only once two analyses agree and is
        // held for a few seconds after it stops arriving; without both, the badge
        // flickers on every noisy window and is unreadable.
        //
        // DCS wins when both are present. A channel carries one or the other, and a
        // locked Golay word is far harder to fake than a spectral peak.
        void tick(const Result* candidates, int candidateCount) {
            std::lock_guard<std::mutex> lck(resultMtx);

            // Of the tones this window showed, the one the squelch would open for takes
            // the confirmation slot, and the loudest takes it when none of them is
            // wanted. Without the preference a louder unrelated peak keeps the slot
            // every window and the tone that was asked for is never counted at all,
            // which is how a receiver with mains hum in the audio ends up unable to
            // open on its own channel's tone. With no squelch target set this is just
            // the loudest peak, as before.
            Result ctcss;
            for (int i = 0; i < candidateCount; i++) {
                if (_target.matches(candidates[i])) {
                    ctcss = candidates[i];
                    break;
                }
            }
            if (ctcss.kind == Result::NONE && candidateCount > 0) { ctcss = candidates[0]; }

            if (ctcss.kind == Result::NONE) {
                ctcssCount = 0;
                ctcssCandidate = Result();
            }
            else if (ctcss == ctcssCandidate) {
                if (ctcssCount < CONFIRMATIONS) { ctcssCount++; }
                // Same tone, so the count stands - but take the new window's reading of
                // where it actually is, smoothed, because each one is a fresh estimate
                // and averaging them beats trusting whichever arrived first.
                if (ctcssCandidate.measuredHz > 0.0f && ctcss.measuredHz > 0.0f) {
                    ctcssCandidate.measuredHz += MEASURED_SMOOTHING * (ctcss.measuredHz - ctcssCandidate.measuredHz);
                }
                else {
                    ctcssCandidate.measuredHz = ctcss.measuredHz;
                }
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
            // a standard tone the target accepts, or the Golay word still has to carry
            // a code it accepts.
            //
            // No fast path for opening. A quarter second window cannot separate 150.0
            // from 151.4 - they are inside each other's mainlobe at that length - so a
            // quick-opening squelch would let the neighbouring tone through. Slower
            // and right beats faster and wrong; the tail is short so speech is not
            // chopped once it is open.
            // Two agreeing words rather than one. A locked decoder produces a valid
            // word on every bit, so the second one costs about 7 ms; noise produces
            // isolated ones, and asking for two in a row that carry the same code took
            // the rate they arm the gate at on a dead channel from a few a minute to
            // none in twenty minutes of it. Nothing like the half second the same
            // requirement costs CTCSS below, which is why that one still opens on one.
            bool dcsMatched = dcsSeenThisTick && dcsCount >= CONFIRMATIONS && _target.matches(dcsCandidate);
            bool ctcssMatched = ctcssCount >= 1 && _target.matches(ctcssCandidate);
            bool matched = dcsMatched || ctcssMatched;

            // An end-of-transmission burst is sent while the tone is still on air, so
            // for a moment after one the analysis still finds the tone that was just
            // said goodbye with. Ignore it for as long as that lasts. Normally the
            // carrier goes within a couple of hundred milliseconds and the tone stops
            // being heard well before the count runs out, which releases it early - the
            // count is only there to bound what a false reading can cost.
            bool tailHeld = false;
            if (tailLockout > 0) {
                if (matched) {
                    tailLockout--;
                    tailHeld = true;
                }
                else {
                    tailLockout = 0;
                }
            }
            // Arms the gate; the fast presence check is what actually holds it
            // open, so this only has to remember that the right tone was identified
            // recently enough for a momentary dropout not to need re-deciding.
            //
            // Latching which code did it is what makes the modes that accept several
            // codes work at all: the fast check needs one frequency to watch, and in
            // those modes the target does not name one.
            if (matched && !tailHeld) {
                // DCS wins when both are present, for the same reason it wins the
                // readout above.
                const Result& src = dcsMatched ? dcsCandidate : ctcssCandidate;
                // Named the way the target names it where the target has a name for
                // it, so the readout echoes back the code the operator selected rather
                // than the other name for the same waveform. ANY mode names nothing,
                // so there the reading itself is the only name available.
                ToneKey k;
                if (!_target.findMatchingKey(src, &k)) { k = ToneKey::fromResult(src); }
                if (!openKeyValid || !(k == openKey)) {
                    openKey = k;
                    openKeyValid = true;
                    // Presence measured for one code says nothing about another.
                    fastPresent = false;
                }
                gateArmed = GATE_ARM_TICKS;
            }
            else if (gateArmed > 0) {
                gateArmed--;
                if (gateArmed == 0) {
                    openKeyValid = false;
                    fastPresent = false;
                }
            }

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
        // 10 dB was under what the loudest bin of pure noise reaches across a band
        // this wide often enough to matter - the largest of a couple of hundred
        // independent bins sits about 9 dB over their median on average, and swings
        // several dB either side of that. 11 dB against a floor measured locally
        // leaves the noise behind without giving up a tone anyone could hear.
        static constexpr float CTCSS_MIN_SNR_DB = 11.0f;
        static constexpr float FLOOR_WINDOW_HZ = 25.0f;
        static constexpr float SNAP_TOLERANCE_HZ = 0.6f; // < half the 1.4 Hz 150.0/151.4 gap
        static constexpr float DC_ALPHA = 0.002f;
        // References either side of the tone, far enough out to clear the mainlobe of
        // a Hann window FAST_WINDOW long, so they measure the noise beside the tone
        // rather than the tone itself.
        static constexpr float FAST_REF_OFFSET_HZ = 20.0f;
        static constexpr float FAST_MIN_RATIO = 4.0f; // 6 dB over the noise beside it
        static constexpr float PI_F = 3.14159265358979f;
        // The flip is 120 degrees at its smallest, so this sits comfortably under the
        // smallest real one and comfortably over what noise puts into the window.
        static constexpr float BURST_PHASE_STEP = 1.57f; // 90 degrees
        static constexpr float BURST_DRIFT_ALPHA = 0.02f;
        static constexpr float BURST_MAG_ALPHA = 0.05f;
        static constexpr float BURST_MIN_MAG_RATIO = 0.4f;
        static const int BURST_SETTLE_BLOCKS = 16; // ~0.5 s for the drift estimate
        static const int TAIL_LOCKOUT_TICKS = 4;   // ~2 s, the cap on a false reading
        static constexpr float DCS_TURNOFF_HZ = 134.4f;
        static const int OPEN_KEY_HOLD_MS = 2000;
        // A locked DCS decoder produces a match every bit, so a quarter second
        // without one means it has stopped.
        static const int DCS_PRESENCE_SAMPLES = 168;
        // The second, third and fourth harmonics. Past that a CTCSS encoder's
        // distortion is far below anything the ear picks out of speech.
        static const int HARMONIC_NOTCHES = 3;
        static constexpr double NOTCH_Q = 15.0;
        static constexpr float NOTCH_RETUNE_HZ = 0.02f;
        static constexpr float MEASURED_SMOOTHING = 0.3f;
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
        // The code the gate is currently open on, which is what the fast check watches
        // and what the UI names. Set from an identification, not from the target.
        ToneKey openKey;
        bool openKeyValid = false;

        // Fast "is that tone still there" monitor, for closing
        std::vector<float> fastHann;
        float fastCoeff[3] = { 0.0f, 0.0f, 0.0f };
        float fastS1[3] = { 0.0f, 0.0f, 0.0f };
        float fastS2[3] = { 0.0f, 0.0f, 0.0f };
        int fastPos = 0;
        float fastTunedTo = 0.0f;
        bool fastPresent = false;
        long long lastDcsMatchSample = 0;

        // End-of-transmission burst detector, and the lockout that keeps the gate shut
        // over the rest of the burst once it has fired.
        bool tailCloseEnabled = true;
        int tailLockout = 0;
        float burstI = 0.0f;
        float burstQ = 0.0f;
        float burstPhase = 0.0f;
        float burstStep = 0.0f;
        float burstTunedTo = 0.0f;
        int burstPos = 0;
        float burstPrev = 0.0f;
        bool burstPrevValid = false;
        float burstDrift = 0.0f;
        float burstMag = 0.0f;
        float burstHist[BURST_HISTORY] = { 0.0f };
        int burstHistPos = 0;
        int burstOverSign = 0;
        int burstSettle = 0;

        // What the gate was last open on, and when, for the readout to keep naming it
        // for a moment after the audio stops.
        ToneKey lastOpenKey;
        bool lastOpenKeyValid = false;
        std::chrono::steady_clock::time_point lastOpenAt;

        // The mono filter rather than FIR<stereo_t, float>, because NFM audio is mono
        // duplicated across both channels: filtering once and copying is half the
        // convolution for the same result. The stereo form is correct now, it is just
        // twice the work here.
        dsp::filter::FIR<float, float> toneFilter;
        ToneNotch harmonicNotch[HARMONIC_NOTCHES];
        float notchTunedTo = 0.0f;
        std::vector<float> filteredBuf;
        dsp::tap<float> filterTaps;
        bool filterEnabled = false;
        bool filterReady = false;
    };
}
