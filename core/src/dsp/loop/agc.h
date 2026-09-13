#pragma once
#include "../processor.h"
#include <algorithm>
#include <cmath>

namespace dsp::loop {
    // Ceiling the look-ahead limiter holds audio AGC output to, as a multiple of the
    // 1.0 set point. A little above it, so ordinary peaks pass untouched and only a
    // gain that wound up during a pause gets pulled back. The demodulators passed
    // 10.0, so far above the set point that the limiter could not engage until the
    // audio was already ten times too loud.
    inline constexpr float AUDIO_AGC_CEILING = 1.5f;

    template <class T>
    class AGC : public Processor<T, T> {
        using base_type = Processor<T, T>;
    public:
        AGC() {}

        AGC(stream<T>* in, double setPoint, double attack, double decay, double maxGain, double maxOutputAmp, double initGain = 1.0) { init(in, setPoint, attack, decay, maxGain, maxOutputAmp, initGain); }

        void init(stream<T>* in, double setPoint, double attack, double decay, double maxGain, double maxOutputAmp, double initGain = 1.0) {
            _setPoint = setPoint;
            _attack = attack;
            _invAttack = 1.0f - _attack;
            _decay = decay;
            _invDecay = 1.0f - _decay;
            _maxGain = maxGain;
            _maxOutputAmp = maxOutputAmp;
            _initGain = initGain;
            _startEnvelope = 0;
            _frozen.store(false);
            amp = startingAmp();
            base_type::init(in);
        }

        void setSetPoint(double setPoint) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _setPoint = setPoint;
        }

        void setAttack(double attack) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _attack = attack;
            _invAttack = 1.0f - _attack;
        }

        bool getAttack() {
            return _attack;
        }

        void setDecay(double decay) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _decay = decay;
            _invDecay = 1.0f - _decay;
        }

        float getDecay() {
            return _decay;
        }

        void setMaxGain(double maxGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _maxGain = maxGain;
        }

        void setMaxOutputAmp(double maxOutputAmp) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _maxOutputAmp = maxOutputAmp;
        }

        void setInitialGain(double initGain) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _initGain = initGain;
        }

        void reset() {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            amp = startingAmp();
            _startEnvelope  =0;
            _hangLeft = 0;
        }

        // How many samples the gain is held for after the signal drops, before the
        // decay is allowed to start raising it. 0, the default, is no hang at all.
        //
        // Without it the gain starts climbing the moment a word ends, so the static
        // in every pause swells up and the next word lands on a gain that has run
        // away. Holding it through a short gap - the length of a pause in speech -
        // keeps the gaps as quiet as the speech around them.
        void setHang(int samples) {
            assert(base_type::_block_init);
            std::lock_guard<std::recursive_mutex> lck(base_type::ctrlMtx);
            _hangSamples = std::max<int>(0, samples);
            _hangLeft = std::min<int>(_hangLeft, _hangSamples);
        }

        void setFrozen(bool b) {
            _frozen.store(b);
        }

        // Where the tracked amplitude starts, which decides the gain of the first
        // samples. The demodulators pass an initial gain of INFINITY, which put this at
        // zero and so started every reset - and every demodulator or AGC mode change -
        // at the maximum gain, a full _maxGain blast of amplified noise before the loop
        // pulled it back. Starting at the set point means starting at unity gain and
        // adapting from there.
        float startingAmp() const {
            if (!(_initGain > 0.0f) || !std::isfinite(_initGain)) { return _setPoint; }
            return _setPoint / _initGain;
        }

        inline int process(int count, T* in, T* out) {
            float envelope = 1.0f;
            if (_attack <= 0) {
                std::copy(in, in + count, out);
                return count;
            }
            // The release must never outrun the attack. Gain rises while the signal is
            // quiet and comes back down when it returns, so a decay faster than the
            // attack winds the gain up between words and then cannot pull it back:
            // the audio comes back far too loud and stays there. The two are set by
            // separate sliders - AM's attack runs 1..200 and its decay 1..20, both
            // divided by the sample rate - so the low end of the attack slider put the
            // loop into exactly that state, which is the "moving the attack slider
            // makes it get louder and louder" bug. Held here rather than in the
            // setters so the values the user chose are still what the menu shows.
            const float decay = std::min<float>(_decay, _attack);
            const float invDecay = 1.0f - decay;

            for (int i = 0; i < count; i++) {
                // Get signal amplitude
                float inAmp, gain = 1.0;
                if constexpr (std::is_same_v<T, complex_t>) {
                    inAmp = in[i].amplitude();
                }
                if constexpr (std::is_same_v<T, float>) {
                    inAmp = fabsf(in[i]);
                }

                // Update average amplitude
                if (inAmp != 0.0f) {
                    if (!_frozen.load()) {
                        float namp;
                        if (inAmp > amp) {
                            // Louder: follow it up, and start the hang over again.
                            namp = (amp * _invAttack) + (inAmp * _attack);
                            _hangLeft = _hangSamples;
                        }
                        else if (_hangLeft > 0) {
                            // Quieter, but still inside the hang: hold the gain.
                            _hangLeft--;
                            namp = amp;
                        }
                        else {
                            namp = (amp * invDecay) + (inAmp * decay);
                        }
                        if (!isnan(namp)) {
                            amp = namp;
                            gain = std::min<float>(_setPoint / amp, _maxGain);
                        }
                    }
                }
                else {
                    gain = 1.0f;
                }

                // If the gain wound up while the signal was quiet, the moment it comes
                // back the output would be far above the set point until the loop
                // catches up - heard as a blast at the start of every over. Look ahead
                // at the rest of the buffer and take the gain straight down to what its
                // loudest sample needs.
                //
                // This was disabled in 2022 (a5c544cd, "removed forced gain guard"),
                // which is what let AM and SSB overshoot the set point by 12-26x after
                // a pause. Measured on speech-shaped audio, re-enabling it with the
                // output ceilings below holds the peak at 1.1-1.4x instead.
                if (inAmp*gain > _maxOutputAmp) {
                    float maxAmp = 0;
                    for (int j = i; j < count; j++) {
                        if constexpr (std::is_same_v<T, complex_t>) {
                            inAmp = in[j].amplitude();
                        }
                        if constexpr (std::is_same_v<T, float>) {
                            inAmp = fabsf(in[j]);
                        }
                        if (inAmp > maxAmp) { maxAmp = inAmp; }
                    }
                    amp = maxAmp;
                    gain = std::min<float>(_setPoint / amp, _maxGain);
                }

                if (_startEnvelope < _totalEnvelopeLength) {
                    envelope = _startEnvelope / (float)_totalEnvelopeLength;
                }
                _startEnvelope++;
                // Scale output by gain
                out[i] = in[i] * gain * envelope;
            }
            return count;
        }

        int run() {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }

            process(count, base_type::_in->readBuf, base_type::out.writeBuf);

            base_type::_in->flush();
            if (!base_type::out.swap(count)) { return -1; }
            return count;
        }

    protected:
        float _setPoint;
        float _attack;
        float _invAttack;
        float _decay;
        float _invDecay;
        float _maxGain;
        float _maxOutputAmp;
        float _initGain;
        int   _startEnvelope;
        int   _totalEnvelopeLength = 4800; // length of start envelope, circa 1/10 of second. This is needed to reduce clicks when e.g. switching SSB/AM
        std::atomic_bool _frozen;
        int   _hangSamples = 0;
        int   _hangLeft = 0;

        float amp = 1.0;

    };
}