#pragma once
#include <dsp/demod/psk.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/stream.h>
#include <dsp/types.h>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include "afc.h"
#include "varicode.h"

namespace psk {
    // Samples per symbol taken out of the VFO. Enough for the clock recovery to
    // interpolate between without making the matched filter enormous: the RRC has to
    // span several symbols, so its tap count scales with this.
    static const int SAMPLES_PER_SYMBOL = 16;

    // How wide the channel filter is by default, as a multiple of the symbol rate. A
    // PSK31 signal is about one baud wide, so most of this is tuning tolerance: room
    // for the carrier to sit off centre and still be inside the filter for the AFC to
    // find it. Adjustable at runtime between these limits - wide enough to catch a
    // signal well off where you clicked, or narrow enough to shut out the station
    // next door on a crowded band.
    //
    // The filter is now the thing that bounds how far off tune a signal can be and
    // still decode: the AFC's own limit is a quarter of the sample rate, which is
    // always wider than this.
    static const double BANDWIDTH_IN_BAUD = 4.0;
    static const double MIN_BANDWIDTH_IN_BAUD = 1.5;
    // Capped by the sample rate, which is SAMPLES_PER_SYMBOL baud wide.
    static const double MAX_BANDWIDTH_IN_BAUD = (double)SAMPLES_PER_SYMBOL;

    struct Mode {
        const char* name;
        double baud;
    };

    // The BPSK speeds. All four are the same modulation and the same varicode; only
    // the symbol rate changes, which is why they cost nothing to support beyond an
    // entry here.
    static const Mode MODES[] = {
        { "BPSK31", 31.25 },
        { "BPSK63", 62.5 },
        { "BPSK125", 125.0 },
        { "BPSK250", 250.0 },
    };
    static const int MODE_COUNT = 4;

    // Turns a VFO's baseband into text.
    //
    // The chain is root raised cosine -> AGC -> Costas -> Mueller and Muller, all of
    // which dsp::demod::PSK already assembles, leaving this class two jobs: work out
    // each symbol's phase relative to the last one, and keep a running measure of how
    // convincingly the symbols are landing where BPSK symbols should.
    class Receiver {
    public:
        // Called from the DSP thread for each decoded character.
        std::function<void(char)> onChar;

        double sampleRateFor(int modeIdx) const {
            return MODES[modeIdx].baud * (double)SAMPLES_PER_SYMBOL;
        }

        // The filter width in hertz for a given mode, at the current setting.
        double bandwidthFor(int modeIdx) const {
            return MODES[modeIdx].baud * bandwidthInBaud;
        }

        double minBandwidthFor(int modeIdx) const {
            return MODES[modeIdx].baud * MIN_BANDWIDTH_IN_BAUD;
        }

        double maxBandwidthFor(int modeIdx) const {
            return MODES[modeIdx].baud * MAX_BANDWIDTH_IN_BAUD;
        }

        // As a multiple of the symbol rate, so the setting means the same thing at
        // every speed and survives a mode change.
        double bandwidthInBaud = BANDWIDTH_IN_BAUD;

        void init(dsp::stream<dsp::complex_t>* in, int modeIdx) {
            _modeIdx = modeIdx;
            double baud = MODES[modeIdx].baud;
            double sr = sampleRateFor(modeIdx);

            // Enough taps for the matched filter to span about four symbols. Odd, so
            // it has a middle sample and adds no fractional delay of its own.
            int taps = (SAMPLES_PER_SYMBOL * 4) | 1;

            // Costas bandwidth is per sample, so it has to shrink as the sample rate
            // rises or a fast mode would track its own noise. Expressed as a fraction
            // of a symbol it is the same loop at every speed.
            //
            // 0.06 rather than the 0.01 this started at. Measured against a generated
            // BPSK31 signal, the narrower loop settled with a standing phase error
            // that pulled the quality metric down to 0.83 on a signal only 3 Hz off
            // tune; at 0.06 the same signal reads 1.00. It buys nothing in noise -
            // both decode a signal buried in sigma 0.7 of it without an error.
            double costasBw = 0.06 / (double)SAMPLES_PER_SYMBOL;

            // Frequency first, then the matched filter. The other way round and the
            // filter has already thrown away what the AFC needs to measure.
            afc.init(in, sr, baud);
            demod.init(&afc.out, baud, sr, taps, 0.5, 0.02f, costasBw, 1e-6, 0.01);
            symSink.init(&demod.out, symbolHandler, this);
        }

        void setInput(dsp::stream<dsp::complex_t>* in) {
            afc.setInput(in);
        }

        void setMode(int modeIdx) {
            _modeIdx = modeIdx;
            afc.setRates(sampleRateFor(modeIdx), MODES[modeIdx].baud);
            demod.setSamplerate(sampleRateFor(modeIdx));
            demod.setSymbolrate(MODES[modeIdx].baud);
            reset();
        }

        void start() {
            afc.start();
            demod.start();
            symSink.start();
        }

        void stop() {
            afc.stop();
            demod.stop();
            symSink.stop();
        }

        void reset() {
            varicode.reset();
            prevSym = dsp::complex_t{ 0.0f, 0.0f };
            quality = 0.0f;
            if (afc.ready()) { afc.reset(); }
        }

        // How far off the VFO centre the signal actually is, in hertz.
        double offsetHz() const { return afc.offsetHz(); }

        // Whether that figure is being measured right now, or is the last one from
        // before the signal went away.
        bool afcTracking() const { return afc.coherence() >= afc.minCoherence; }

        // 0 to 1, and about 0.64 on noise. Measures the same thing the bit decision
        // does: how nearly each symbol lies along the previous one's axis, whichever
        // way round. On a clean signal consecutive symbols are either aligned or
        // exactly opposed, so this sits near 1; on noise the angle between them is
        // spread evenly around the circle and the mean of |cos| is 2/pi.
        //
        // Measuring against the real axis instead - which is what this did first -
        // looks equivalent and is not. The Costas loop settles with a standing phase
        // error whenever the signal is off frequency, which rotates the whole
        // constellation. Differential decoding does not care, so the text stayed
        // perfect, but the metric collapsed and the squelch below it threw away every
        // character of a signal that was decoding correctly.
        float getQuality() const { return quality; }

        // Below this, characters are dropped rather than printed. Without it an empty
        // channel prints a steady drizzle of random letters, which is worse than
        // printing nothing because it looks like a signal.
        //
        // 0.70 leaves a clear margin over the 0.637 a noise-only channel sits at,
        // while staying under what a signal reads even when it is off tune enough to
        // be losing characters anyway.
        float squelch = 0.70f;

        // The last SYMBOL_MEMORY symbols, oldest first, for the constellation plot.
        // Copied out under the lock rather than handed over, because the DSP thread
        // goes on writing into the ring while the UI draws.
        static const int SYMBOL_MEMORY = 1024;
        void copySymbols(dsp::complex_t* dst) {
            std::lock_guard<std::mutex> lck(symMtx);
            for (int i = 0; i < SYMBOL_MEMORY; i++) {
                dst[i] = symbols[(symPos + i) % SYMBOL_MEMORY];
            }
        }

        AFC afc;
        dsp::demod::PSK<2> demod;

    private:
        static void symbolHandler(dsp::complex_t* data, int count, void* ctx) {
            Receiver* _this = (Receiver*)ctx;
            {
                std::lock_guard<std::mutex> lck(_this->symMtx);
                for (int i = 0; i < count; i++) {
                    _this->symbols[_this->symPos] = data[i];
                    _this->symPos = (_this->symPos + 1) % SYMBOL_MEMORY;
                }
            }

            for (int i = 0; i < count; i++) {
                dsp::complex_t sym = data[i];
                dsp::complex_t prev = _this->prevSym;

                // PSK31 is differential: the bit is in the change from the previous
                // symbol, not in the symbol itself. That is what makes the Costas
                // loop's 180 degree ambiguity harmless - both lock points decode the
                // same text - and what makes a standing phase error harmless too.
                float dot = (sym.re * prev.re) + (sym.im * prev.im);
                _this->prevSym = sym;

                float magCur = sqrtf((sym.re * sym.re) + (sym.im * sym.im));
                float magPrev = sqrtf((prev.re * prev.re) + (prev.im * prev.im));
                if (magCur > 1e-6f && magPrev > 1e-6f) {
                    // Exponential average over roughly the last thirty symbols. Long
                    // enough to be steady on screen and short enough that the squelch
                    // opens about a second after a transmission starts - at 0.01, a
                    // hundred symbols, the first eight to twenty characters of every
                    // over were thrown away before the metric had climbed past the
                    // threshold.
                    float inst = fabsf(dot) / (magCur * magPrev);
                    _this->quality += (inst - _this->quality) * 0.03f;
                }

                // No reversal is a 1, a reversal is a 0.
                int bit = (dot >= 0.0f) ? 1 : 0;

                char c = _this->varicode.put(bit);
                if (c && _this->quality >= _this->squelch && _this->onChar) {
                    _this->onChar(c);
                }
            }
        }

        dsp::sink::Handler<dsp::complex_t> symSink;
        VaricodeDecoder varicode;
        dsp::complex_t prevSym = { 0.0f, 0.0f };
        float quality = 0.0f;
        int _modeIdx = 0;

        std::mutex symMtx;
        dsp::complex_t symbols[SYMBOL_MEMORY] = {};
        int symPos = 0;
    };
}
