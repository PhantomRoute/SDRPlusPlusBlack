#pragma once
#include "../processor.h"
#include "../window/nuttall.h"
#include <utils/arrays.h>
#include <algorithm>

namespace dsp::noise_reduction {
    // Gate that mutes the stream while the signal stays below a threshold.
    //
    // On the complex path the measured level is the peak of the channel's power
    // spectrum, normalised exactly the way the waterfall normalises its own FFT
    // (|X|^2/N^2, with an unnormalised window). That is what the eye reads off
    // the waterfall: the top of the signal, not its average. It is a per-bin
    // level though, so a caller comparing it against the waterfall still has to
    // account for the two FFTs having different resolutions -- see
    // RadioModule::updateSquelchScaling().
    //
    // The stereo path is a plain RMS level in amplitude dBFS. It feeds the mic
    // squelch, where there is no spectrum on screen to agree with.
    class Squelch : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        // Resolution of the internal channel spectrum, in bins. Deliberately
        // coarse: the level only has to track the shape of the channel, and a
        // small transform lets a single input block average many of them
        // together, which is what keeps the level steady.
        static constexpr int SPECTRUM_SIZE = 128;

        Squelch() {}

        Squelch(stream<complex_t>* in, double level) {}

        ~Squelch() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
        }

        void init(stream<complex_t>* in, double level) {
            _level = level;

            fftPlan = arrays::allocateFFTWPlan(false, SPECTRUM_SIZE);
            fftIn = arrays::npzeros_c(SPECTRUM_SIZE);
            accBuf.resize(SPECTRUM_SIZE);
            specWindow.resize(SPECTRUM_SIZE);
            specPower.resize(SPECTRUM_SIZE);

            double gain = 0;
            for (int i = 0; i < SPECTRUM_SIZE; i++) {
                specWindow[i] = window::nuttall(i, SPECTRUM_SIZE);
                gain += specWindow[i] * specWindow[i];
            }
            _windowNoiseGain = gain / (double)SPECTRUM_SIZE;

            base_type::init(in);
        }

        void setLevel(double level) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _level = level;
        }

        // Amount the level has to fall back below the threshold before an open
        // squelch closes again. Keeps a signal sitting right on the limit from
        // chopping the audio into pieces.
        void setHysteresis(double hysteresis) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _hysteresis = hysteresis;
        }

        // Sum of the squared spectrum window coefficients over SPECTRUM_SIZE.
        double getWindowNoiseGain() { return _windowNoiseGain; }

        // Level of the last processed block, in the same units as setLevel().
        float getMeasuredLevel() { return _lastLevel; }

        bool isOpen() { return _open; }

        inline int process(int count, const stereo_t* in, stereo_t* out) {
            if (count <= 0) {
                return count;
            }
            float sum = 0;
            for(int i=0; i<count; i++) {
                sum += in[i].l * in[i].l;
                sum += in[i].r * in[i].r;
            }
            gate(10.0f * log10f((sum / (float)(count * 2)) + 1e-30f));

            if (_open) {
                // Callers may filter in place, in which case there is nothing to copy.
                if (in != out) { memcpy(out, in, count * sizeof(stereo_t)); }
            }
            else {
                memset(out, 0, count * sizeof(stereo_t));
            }

            return count;
        }

        inline int process(int count, const complex_t* in, complex_t* out) {
            if (count <= 0) {
                return count;
            }
            gate(measurePeak(count, in));

            if (_open) {
                if (in != out) { memcpy(out, in, count * sizeof(complex_t)); }
            }
            else {
                memset(out, 0, count * sizeof(complex_t));
            }

            return count;
        }

        //DEFAULT_PROC_RUN();

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            process(count, base_type::_in->readBuf, base_type::out.writeBuf);
            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    private:
        // Averages the periodogram of every whole transform that fits in this
        // block and returns the level of its loudest bin. Samples left over are
        // carried into the next call, so short blocks still make progress.
        float measurePeak(int count, const complex_t* in) {
            std::fill(specPower.begin(), specPower.end(), 0.0f);
            int frames = 0;

            for (int pos = 0; pos < count;) {
                int take = (std::min)(SPECTRUM_SIZE - accPos, count - pos);
                memcpy(&accBuf[accPos], &in[pos], take * sizeof(complex_t));
                accPos += take;
                pos += take;
                if (accPos < SPECTRUM_SIZE) { break; }
                accPos = 0;

                complex_t* fin = fftIn->data();
                for (int i = 0; i < SPECTRUM_SIZE; i++) {
                    fin[i] = accBuf[i] * specWindow[i];
                }
                const complex_t* fout = fftPlan->npfftfft(fftIn)->data();
                for (int i = 0; i < SPECTRUM_SIZE; i++) {
                    specPower[i] += (fout[i].re * fout[i].re) + (fout[i].im * fout[i].im);
                }
                frames++;
            }

            if (frames == 0) { return _lastLevel; }

            float peak = 0;
            for (int i = 0; i < SPECTRUM_SIZE; i++) {
                if (specPower[i] > peak) { peak = specPower[i]; }
            }

            // Same scaling the waterfall applies to its own FFT, so the two are
            // on one scale once the resolution difference is taken out.
            const float norm = 1.0f / ((float)frames * (float)SPECTRUM_SIZE * (float)SPECTRUM_SIZE);
            return 10.0f * log10f((peak * norm) + 1e-30f);
        }

        inline void gate(float level) {
            _lastLevel = level;
            _open = _open ? (level >= _level - _hysteresis) : (level >= _level);
        }

        arrays::Arg<arrays::FFTPlan> fftPlan;
        arrays::ComplexArray fftIn;
        std::vector<complex_t> accBuf;
        std::vector<float> specWindow;
        std::vector<float> specPower;
        int accPos = 0;
        double _windowNoiseGain = 1.0;

        float _level = -50.0f;
        float _hysteresis = 1.5f;
        float _lastLevel = -300.0f;
        bool _open = false;

    };
}
