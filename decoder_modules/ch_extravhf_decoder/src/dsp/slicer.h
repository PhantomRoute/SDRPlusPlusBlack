#pragma once
#include <dsp/processor.h>
#include <dsp/multirate/polyphase_bank.h>
#include <dsp/loop/phase_control_loop.h>
#include <dsp/taps/windowed_sinc.h>
#include <dsp/filter/fir.h>
#include <dsp/loop/agc.h>
#include <dsp/loop/costas.h>
#include <dsp/loop/fast_agc.h>
#include <dsp/taps/root_raised_cosine.h>

namespace dsp {

    #define LVL_BUFF 1024

    class FourFSKExtractor : public Processor<float, uint8_t> {
        using base_type = Processor<float, uint8_t>;
    public:
        FourFSKExtractor() {}

        FourFSKExtractor(stream<float>* in) {init(in);}

        inline int process(int count, const float* in, uint8_t* out) {
            for(int i = 0; i < count; i++) {
                // Turning the symbols over here rather than anywhere downstream is
                // what makes one switch enough: every frame sync pattern, every
                // dibit and every protocol after this point then sees the signal the
                // right way up, with no second copy of any of them.
                float sym_c = inverted ? -in[i] : in[i];
                if(sym_c >= umid) {
                    out[i] = 0b01;
                } else if(sym_c >= center) {
                    out[i] = 0b00;
                } else if(sym_c >= lmid) {
                    out[i] = 0b10;
                } else {
                    out[i] = 0b11;
                }
                lbuf2[lbuf2idx] = sym_c;
                lbuf2idx = (lbuf2idx+1) % LVL_BUFF;
                // Scan the 24 most recent symbols. This used to read lbuf2[0..23]
                // regardless of where the write index was, and since that wraps at
                // LVL_BUFF those slots are only rewritten once every 1024 symbols -
                // so the window was stale, and before the first wrap it was reading
                // an uninitialised array. Which is very likely why the level tracking
                // below was left switched off: it was being driven by noise.
                float lmax = 0;
                float lmin = 0;
                for(int j = 0; j < 24; j++) {
                    float v = lbuf2[(lbuf2idx + LVL_BUFF - 1 - j) % LVL_BUFF];
                    if(v > lmax) {
                        lmax = v;
                    }
                    if(v < lmin) {
                        lmin = v;
                    }
                }
                // Follow the levels the symbols are actually arriving at. This was
                // switched off upstream - with good reason at the time, because the
                // window above was being read from stale slots, so it was tracking
                // noise rather than the signal.
                //
                // With the levels nailed to +-1 the slicer is assuming the
                // transmitter runs exactly 1944 Hz of deviation and that the chain in
                // front of it has unity gain. Measured against the known frame sync
                // symbols of a live DMR channel, a signal 15% low in deviation read
                // 10-15% of its dibits wrong and one 25% low read a third of them
                // wrong, where following the levels reads well under 1%. At the
                // nominal deviation following them is no worse, and better once there
                // is noise on the signal: 0.5% against 1.8% at 15 dB.
                max += (lmax - max) * LEVEL_TRACK_RATE;
                min += (lmin - min) * LEVEL_TRACK_RATE;
                // Held inside a sane range. The outer limit was 1.3 while the levels
                // were fixed, where it only ever clamped a number nothing wrote to;
                // now that they follow the signal it is a real ceiling, and 1.3 is
                // too low. The quadrature demodulator ahead of this maps 1944 Hz of
                // deviation to 1.0, and a transmitter sitting well above that is
                // ordinary - the DMR recording in the e2e set arrives at around 1.8 -
                // so a ceiling of 1.3 would peg the thresholds and lose the tracking
                // exactly as the fixed levels did, only at the other end. 3.0 is far
                // enough out to leave real signals alone and still stop a spike
                // dragging the thresholds past anything a symbol reaches.
                //
                // The inner limit stops them collapsing onto zero in the gaps between
                // transmissions, which would leave the slicer unable to tell one
                // symbol from another until it had tracked back up again - and the
                // frame sync it needs to do that arrives in the first few symbols.
                if(max > 3.0f) { max = 3.0f; }
                else if(max < 0.35f) { max = 0.35f; }
                if(min < -3.0f) { min = -3.0f; }
                else if(min > -0.35f) { min = -0.35f; }
                center = ((max) + (min)) * 0.5f;
                umid = (((max) - center) * mid) + center;
                lmid = (((min) - center) * mid) + center;
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            int outCount = process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            // Swap if some data was generated
            base_type::_in->flush();
            if (outCount) {
                if (!base_type::out.swap(outCount)) { return -1; }
            }
            return outCount;
        }

        float max = 1.0f;
        float min = -1.0f;
        float center = 0.0f;
        float umid = 0.625f;
        float lmid = -0.625f;
        // Where the inner/outer boundary sits between the tracked levels. Left at
        // upstream's value: lmax and lmin above are the single largest and smallest
        // of 24 symbols rather than an average, so they read a little beyond the
        // real outer levels, and 0.6 of an estimate that runs high lands near the
        // two thirds an exact +-3/+-1 constellation wants.
        float mid = 0.6f;
        // Whether the signal arrives with its deviation the wrong way round, which
        // happens when I and Q are swapped somewhere ahead of the receiver. A 4FSK
        // frame sync read upside down still matches - DMR's data and voice sync
        // words are exact inversions of each other - so the decoder syncs, says so,
        // and then reads every dibit inverted. Off by default; there is nothing to
        // detect it from automatically that does not also fire on a good signal.
        bool inverted = false;
        // How fast the levels follow, per symbol. Slow enough that the data pattern
        // in one burst does not move them, fast enough to be there within a frame.
        static constexpr float LEVEL_TRACK_RATE = 0.01f;

    private:
        int lbuf2idx = 0;
        // Needs initialising: the level scan above reads it before a full pass has
        // been written.
        float lbuf2[LVL_BUFF] = {};

    };
}
