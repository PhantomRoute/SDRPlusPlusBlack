#pragma once
#include <dsp/processor.h>
#include <dsp/types.h>
#include <cmath>

namespace psk {
    // Frequency acquisition, ahead of the matched filter.
    //
    // Why it has to be ahead of it: the matched filter is about one baud wide and
    // sits at DC, so a carrier more than a few hertz off tune is attenuated and
    // distorted before the carrier recovery ever looks at it. Measured against
    // generated PSK31, the chain without this decoded perfectly on tune, lost most of
    // a message at 5 Hz and produced nothing at all at 10 Hz. Widening the Costas
    // loop does not help, and neither does widening the filter - by the time either
    // one looks, the signal it needs has already been filtered away.
    //
    // How it works: squaring a BPSK signal removes the modulation, because both
    // symbol values square to the same thing, and leaves a tone at twice the carrier
    // offset. The phase that tone advances from one sample to the next is
    // 2*(2*pi*f/sr), so averaging x[n]^2 * conj(x[n-1]^2) over a window and taking
    // the argument measures the offset directly - no FFT, no search, and samples
    // where the envelope has dipped to zero drop out of the average by themselves
    // because they carry no phase. That estimate steers an NCO, closing a first order
    // loop around the whole thing.
    //
    // Unambiguous while |arg| < pi, which is |f| < samplerate/4. That is wider than
    // the channel filter at any setting, so in practice the filter is the limit and
    // this is not. With it, the same generated signals decode out to at least +/-55
    // Hz and through a drift of 1 Hz per second.
    class AFC : public dsp::Processor<dsp::complex_t, dsp::complex_t> {
        using base_type = dsp::Processor<dsp::complex_t, dsp::complex_t>;

    public:
        AFC() {}

        void init(dsp::stream<dsp::complex_t>* in, double sampleRate, double baud) {
            setRates(sampleRate, baud);
            resetState();
            base_type::init(in);
        }

        void setRates(double sampleRate, double baud) {
            _sr = sampleRate;
            // Average over about eight symbols: long enough to be steady on a noisy
            // signal, short enough to follow one that is drifting.
            _window = (int)((sampleRate / baud) * 8.0);
            if (_window < 16) { _window = 16; }
        }

        // The base class asserts on most calls before init, and has no public way to
        // ask, so callers that may run either side of it need this.
        bool ready() const { return base_type::_block_init; }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            resetState();
            base_type::tempStart();
        }

        // What is currently being taken out, in hertz. Signed, and worth showing:
        // it is the one number that says whether the VFO is actually on the signal.
        double offsetHz() const { return ncoFreq * _sr / (2.0 * M_PI); }

        inline int process(int count, const dsp::complex_t* in, dsp::complex_t* out) {
            for (int i = 0; i < count; i++) {
                float cs = cosf((float)-ncoPhase);
                float sn = sinf((float)-ncoPhase);
                dsp::complex_t c;
                c.re = (in[i].re * cs) - (in[i].im * sn);
                c.im = (in[i].re * sn) + (in[i].im * cs);
                out[i] = c;

                ncoPhase += ncoFreq;
                if (ncoPhase > M_PI) { ncoPhase -= 2.0 * M_PI; }
                if (ncoPhase < -M_PI) { ncoPhase += 2.0 * M_PI; }

                dsp::complex_t sq;
                sq.re = (c.re * c.re) - (c.im * c.im);
                sq.im = 2.0f * c.re * c.im;
                if (havePrev) {
                    // sq * conj(prevSq), accumulated, along with the total length of
                    // the terms that went into it.
                    accRe += ((double)sq.re * prevSq.re) + ((double)sq.im * prevSq.im);
                    accIm += ((double)sq.im * prevSq.re) - ((double)sq.re * prevSq.im);
                    accMag += sqrt((double)((sq.re * sq.re) + (sq.im * sq.im))) *
                              sqrt((double)((prevSq.re * prevSq.re) + (prevSq.im * prevSq.im)));
                    samples++;
                }
                prevSq = sq;
                havePrev = true;

                if (samples >= _window) {
                    // How much the terms agreed with each other. Terms that all point
                    // the same way - a real signal turning at a steady rate - sum to
                    // very nearly their combined length, so this approaches 1. Noise
                    // points every way at once and largely cancels, so this falls
                    // towards zero.
                    //
                    // Without this gate the loop believed the noise. On a signal well
                    // buried in it but correctly tuned, the estimate wandered several
                    // hertz off and dragged the signal out of the filter: a case that
                    // decoded in full before the AFC existed came back in pieces. The
                    // right behaviour when the measurement means nothing is to leave
                    // the frequency alone, not to act on it.
                    double mag = sqrt((accRe * accRe) + (accIm * accIm));
                    double coherence = (accMag > 0.0) ? (mag / accMag) : 0.0;
                    if (coherence >= minCoherence) {
                        // Halved: the measurement was made on the squared signal.
                        double err = atan2(accIm, accRe) * 0.5;
                        ncoFreq += err * gain;
                        // Past a quarter of the sample rate the measurement wraps and
                        // stops meaning anything, so do not let the loop wander there.
                        const double lim = M_PI / 4.0;
                        if (ncoFreq > lim) { ncoFreq = lim; }
                        if (ncoFreq < -lim) { ncoFreq = -lim; }
                    }
                    lastCoherence = (float)coherence;
                    accRe = accIm = accMag = 0.0;
                    samples = 0;
                }
            }
            return count;
        }

        DEFAULT_PROC_RUN

        double gain = 0.20;
        // Below this the window's terms disagree too much for the estimate to be
        // worth acting on.
        //
        // Chosen by sweeping it against generated signals, against one rule: a signal
        // that is already tuned correctly must never decode worse with this loop than
        // without it. At 0.45 a weak on-tune signal that used to decode in full came
        // back two thirds complete, because the loop was still believing noise. At
        // 0.55 it is whole again and everything off tune still works. Higher than
        // that and the gate stops opening at all on a weak signal, so a badly tuned
        // weak one gets no correction rather than an imperfect one.
        double minCoherence = 0.55;

        // What the last window scored, for the panel to show.
        float coherence() const { return lastCoherence; }

    private:
        void resetState() {
            ncoPhase = 0.0;
            ncoFreq = 0.0;
            prevSq = { 0.0f, 0.0f };
            accRe = accIm = accMag = 0.0;
            lastCoherence = 0.0f;
            samples = 0;
            havePrev = false;
        }

        double _sr = 1.0;
        int _window = 128;
        double ncoPhase = 0.0;
        double ncoFreq = 0.0;
        dsp::complex_t prevSq = { 0.0f, 0.0f };
        double accRe = 0.0, accIm = 0.0, accMag = 0.0;
        float lastCoherence = 0.0f;
        int samples = 0;
        bool havePrev = false;
    };
}
