#pragma once
#include "../processor.h"

namespace dsp::noise_reduction {
    // Gate that mutes the stream while its level stays below a threshold.
    //
    // The level is the mean power of a block expressed as amplitude dBFS
    // (0dB == full scale sine), i.e. 20*log10 of the RMS amplitude. For the
    // complex path that is the total power inside the channel, summed over the
    // whole VFO bandwidth -- it is NOT the per-FFT-bin level the waterfall
    // draws. Callers that let the user pick the threshold off the waterfall
    // have to convert (see RadioModule::updateSquelchScaling).
    class Squelch : public Processor<complex_t, complex_t> {
        using base_type = Processor<complex_t, complex_t>;
    public:
        Squelch() {}

        Squelch(stream<complex_t>* in, double level) {}

        ~Squelch() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            buffer::free(normBuffer);
        }

        void init(stream<complex_t>* in, double level) {
            _level = level;

            normBuffer = buffer::alloc<float>(STREAM_BUFFER_SIZE);

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
            float sum;
            volk_32fc_magnitude_squared_32f(normBuffer, (const lv_32fc_t*)in, count);
            volk_32f_accumulator_s32f(&sum, normBuffer, count);
            gate(10.0f * log10f((sum / (float)count) + 1e-30f));

            if (_open) {
                if (in != out) { memcpy(out, in, count * sizeof(complex_t)); }
            }
            else {
                memset(out, 0, count * sizeof(complex_t));
            }

            return count;
        }

        // Level of the last processed block, in the same units as setLevel().
        float getMeasuredLevel() { return _lastLevel; }

        bool isOpen() { return _open; }

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
        inline void gate(float level) {
            _lastLevel = level;
            _open = _open ? (level >= _level - _hysteresis) : (level >= _level);
        }

        float* normBuffer = nullptr;
        float _level = -50.0f;
        float _hysteresis = 1.5f;
        float _lastLevel = -300.0f;
        bool _open = false;

    };
}
