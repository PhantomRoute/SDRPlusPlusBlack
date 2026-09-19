#pragma once
#include "../processor.h"
#include "../loop/phase_control_loop.h"
#include "../taps/windowed_sinc.h"
#include "../multirate/polyphase_bank.h"

namespace dsp::clock_recovery {
    /**
     * Gardner timing recovery.
     *
     * The error term is the sample half a symbol back multiplied by the change
     * between the last symbol and this one. At the right instant the midpoint sits
     * on the crossing between the two symbols and reads zero however far apart they
     * are, so the detector says nothing about the amplitude and everything about the
     * timing. That is what makes it the one to use for a signal with more than two
     * levels: FD, which walks towards a peak of the waveform, is only correct when
     * every symbol is a peak. On 4FSK the inner levels are not - in a run like
     * +3, +1, +3 the middle symbol sits in a trough - so FD pushes away from the
     * centre of exactly the symbols that have the least margin to give.
     *
     * Needs at least two samples per symbol, since it has to see the midpoint.
     */
    class Gardner : public Processor<float, float> {
        using base_type = Processor<float, float>;
    public:
        Gardner() {}

        Gardner(stream<float>* in, double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8) { init(in, omega, omegaGain, muGain, omegaRelLimit, interpPhaseCount, interpTapCount); }

        ~Gardner() {
            if (!base_type::_block_init) { return; }
            base_type::stop();
            dsp::multirate::freePolyphaseBank(interpBank);
            buffer::free(buffer);
        }

        void init(stream<float>* in, double omega, double omegaGain, double muGain, double omegaRelLimit, int interpPhaseCount = 128, int interpTapCount = 8) {
            assert(omega >= 2.0);
            _omega = omega;
            _omegaGain = omegaGain;
            _muGain = muGain;
            _omegaRelLimit = omegaRelLimit;
            _interpPhaseCount = interpPhaseCount;
            _interpTapCount = interpTapCount;

            pcl.init(_muGain, _omegaGain, 0.0, 0.0, 1.0, _omega, _omega * (1.0 - omegaRelLimit), _omega * (1.0 + omegaRelLimit));
            generateInterpTaps();
            buffer = buffer::alloc<float>(STREAM_BUFFER_SIZE + _interpTapCount);
            bufStart = &buffer[_interpTapCount - 1];

            base_type::init(in);
        }

        void setOmega(double omega) {
            assert(base_type::_block_init);
            assert(omega >= 2.0);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            _omega = omega;
            offset = 0;
            pcl.phase = 0.0f;
            pcl.freq = _omega;
            pcl.setFreqLimits(_omega * (1.0 - _omegaRelLimit), _omega * (1.0 + _omegaRelLimit));
            lastSym = 0.0f;
            base_type::tempStart();
        }

        void setOmegaGain(double omegaGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _omegaGain = omegaGain;
            pcl.setCoefficients(_muGain, _omegaGain);
        }

        void setMuGain(double muGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _muGain = muGain;
            pcl.setCoefficients(_muGain, _omegaGain);
        }

        void setOmegaRelLimit(double omegaRelLimit) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _omegaRelLimit = omegaRelLimit;
            pcl.setFreqLimits(_omega * (1.0 - _omegaRelLimit), _omega * (1.0 + _omegaRelLimit));
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            base_type::tempStop();
            offset = 0;
            pcl.phase = 0.0f;
            pcl.freq = _omega;
            lastSym = 0.0f;
            base_type::tempStart();
        }

        inline int process(int count, const float* in, float* out) {
            // Copy data to work buffer
            memcpy(bufStart, in, count * sizeof(float));

            int outCount = 0;
            while (offset < count) {
                // The symbol itself
                int phase = std::clamp<int>(floorf(pcl.phase * (float)_interpPhaseCount), 0, _interpPhaseCount - 1);
                float symVal;
                volk_32f_x2_dot_prod_32f(&symVal, &buffer[offset], interpBank.phases[phase], _interpTapCount);
                out[outCount++] = symVal;

                // Half a symbol back. Not a whole sample back in general, so the
                // integer and the fractional part of the step are both needed. At the
                // very start of a block that can land before anything we still hold,
                // in which case this one symbol contributes no correction rather than
                // reading off the front of the buffer.
                double midPos = (double)offset + (double)pcl.phase - ((double)pcl.freq * 0.5);
                int midOffset = (int)floor(midPos);
                float midVal = 0.0f;
                if (midOffset >= 0) {
                    float midFrac = (float)(midPos - (double)midOffset);
                    int midPhase = std::clamp<int>(floorf(midFrac * (float)_interpPhaseCount), 0, _interpPhaseCount - 1);
                    volk_32f_x2_dot_prod_32f(&midVal, &buffer[midOffset], interpBank.phases[midPhase], _interpTapCount);
                }

                // The sign is set by which way round the polyphase bank runs:
                // buildPolyphaseBank fills phase p from tap (phaseCount-1 - p), so a
                // larger phase index is an earlier sampling instant, not a later one.
                float error = midVal * (lastSym - symVal);
                lastSym = symVal;

                // Clamp symbol phase error
                if (error > 1.0f) { error = 1.0f; }
                if (error < -1.0f) { error = -1.0f; }

                // Advance symbol offset and phase
                pcl.advance(error);
                float delta = floorf(pcl.phase);
                offset += delta;
                pcl.phase -= delta;
            }
            offset -= count;

            // Update delay buffer
            memmove(buffer, &buffer[count], (_interpTapCount - 1) * sizeof(float));

            return outCount;
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

        loop::PhaseControlLoop<float, false> pcl;

    protected:
        void generateInterpTaps() {
            double bw = 0.5 / (double)_interpPhaseCount;
            dsp::tap<float> lp = dsp::taps::windowedSinc<float>(_interpPhaseCount * _interpTapCount, dsp::math::hzToRads(bw, 1.0), dsp::window::nuttall, _interpPhaseCount);
            interpBank = dsp::multirate::buildPolyphaseBank<float>(_interpPhaseCount, lp);
            taps::free(lp);
        }

        dsp::multirate::PolyphaseBank<float> interpBank;

        double _omega;
        double _omegaGain;
        double _muGain;
        double _omegaRelLimit;
        int _interpPhaseCount;
        int _interpTapCount;

        int offset = 0;
        float lastSym = 0.0f;
        float* buffer;
        float* bufStart;
    };
}
