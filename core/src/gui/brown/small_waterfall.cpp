
#include "small_waterfall.h"

#include <algorithm>
#include <vector>
#include <mutex>
#include <cstddef>
#include <cmath>
#include <dsp/types.h>
#include <dsp/multirate/rational_resampler.h>
#include <gui/widgets/waterfall.h>
#include <fftw3.h>

struct SubWaterfall::SubWaterfallPrivate {
    SubWaterfall *pub;
    dsp::multirate::RationalResampler<dsp::stereo_t> res;
    std::vector<dsp::stereo_t> inputBuffer;
    std::mutex inputBufferMutex;
    std::vector<dsp::stereo_t> resampledV;
    std::vector<std::pair<float, float>> minMaxQueue;
    std::vector<float> levelScratch;
    ImGui::WaterFall waterfall;
    fftwf_complex* fft_in;
    fftwf_complex* fft_out;
    fftwf_plan fftwPlan;
    float* spectrumLine;
    float hiFreq = 5000;
    int fftSize;
    float waterfallRate = 10;
    int sampleRate;
    std::string lbl;


    void flushDrawUpdates() {
        std::lock_guard<std::mutex> lock(inputBufferMutex);

        // The audio reader thread feeds inputBuffer whether or not this waterfall is
        // on screen, and this is the only thing that drains it, so the first draw
        // after it is switched on can face everything recorded since the radio
        // started. Throw all but the most recent rows away in one erase: a waterfall
        // only shows so many lines, the old ones would scroll straight off, and
        // grinding through the backlog a row at a time is quadratic because each row
        // erases from the front.
        const std::ptrdiff_t maxBacklog = (std::ptrdiff_t)fftSize * 64;
        if ((std::ptrdiff_t)inputBuffer.size() > maxBacklog) {
            inputBuffer.erase(inputBuffer.begin(), inputBuffer.end() - maxBacklog);
        }

        while (inputBuffer.size() > fftSize) {
            for (int i = 0; i < fftSize; i++) {
                fft_in[i][0] = inputBuffer[i].l;
                fft_in[i][1] = 0;
            }
            fftwf_execute(fftwPlan);
            volk_32fc_s32f_power_spectrum_32f(spectrumLine, (const lv_32fc_t*)fft_out, fftSize, fftSize);
            auto mx = -5000.0;
            for(int i=0; i<fftSize; i++) {
                if(spectrumLine[i] > mx) {
                    mx = spectrumLine[i];
                }
            }
            pub->peaks.emplace_back(mx);
            while(pub->peaks.size() > 1000) {
                pub->peaks.erase(pub->peaks.begin());
            }
            float* dest = waterfall.getFFTBuffer();
            if (dest == NULL) {
                // The waterfall allocates its FFT storage when it is first drawn and
                // sized, so this is null until then - and memcpy'ing into it is what
                // crashed the moment the audio waterfall became visible. Leave the
                // row in the buffer and pick it up next frame. getFFTBuffer only
                // advances its line counter when it hands back a real buffer, so
                // stopping here costs nothing.
                break;
            }
            memcpy(dest, spectrumLine, fftSize * sizeof(float));
            for (int q = 0; q < fftSize / 2; q++) {
                std::swap(dest[q], dest[q + fftSize / 2]);
            }
            // bins are located
            // -hiFreq .. 0 ... hiFreq  // total fftSize
            //
            // The levels come from the middle of the span, but not from the very
            // middle. "Remove tone from audio" high passes the audio at 300 Hz, and
            // that filter is a windowed sinc a couple of thousand taps long - it is
            // 100 dB down by 150 Hz and bottoms out near -240 dB at DC. The window
            // this used to measure was the middle 1/8th, +/-620 Hz, so with the tone
            // filter on, half the bins it read sat inside that canyon. minn came back
            // at the bottom of it, setWaterfallMin followed it down, and everything
            // that was really there got painted at the top of the palette.
            //
            // So skip a guard either side of DC, wide enough to clear the filter's
            // transition, and read across the speech band instead. And take the floor
            // from a low percentile rather than the outright minimum, so that any
            // other narrow notch - the IF notch filter landing in the audio, a
            // carrier null - cannot drag the scale down the same way.
            const float LEVEL_DC_GUARD_HZ = 450.0f; // clears the corner and its transition
            const float LEVEL_BAND_HZ = 2500.0f;
            const float binWidth = (2.0f * hiFreq) / (float)fftSize;
            int guardBins = (int)ceilf(LEVEL_DC_GUARD_HZ / binWidth);
            // Parenthesised, here and below: this file is compiled into core, which
            // pulls in windows.h, where min and max are function-like macros - and
            // std::min(a, b) then expands to std::(a, b). The extra brackets stop the
            // macro matching. Same reason the tone detector spells its own out longhand.
            int bandBins = (std::min)((int)(LEVEL_BAND_HZ / binWidth), fftSize / 2);
            levelScratch.clear();
            if (bandBins - guardBins >= 4) {
                for (int q = guardBins; q < bandBins; q++) {
                    levelScratch.push_back(dest[fftSize / 2 - q]);
                    levelScratch.push_back(dest[fftSize / 2 + q]);
                }
            }
            else {
                // Too narrow a span to be picky - the middle 1/8th, as it was before,
                // and at least the one bin in the middle however small fftSize is.
                int half = (std::max)(fftSize / 16, 1);
                int from = (std::max)(fftSize / 2 - half, 0);
                int to = (std::min)(fftSize / 2 + half, fftSize);
                for (int q = from; q < to; q++) {
                    levelScratch.push_back(dest[q]);
                }
            }
            float maxx = *std::max_element(levelScratch.begin(), levelScratch.end());
            size_t floorAt = levelScratch.size() / 10;
            std::nth_element(levelScratch.begin(), levelScratch.begin() + floorAt, levelScratch.end());
            float minn = levelScratch[floorAt];
            // With squelch closed the samples are all zero, and the power spectrum of
            // silence comes out at -inf, or a nonsense value once volk is done with
            // it. Averaging that into the running levels drags the colour scale far
            // below anything real, so when the next transmission opens the squelch
            // its first frames map to the top of the palette and arrive as a block of
            // red. Silent lines are still drawn, they just do not get a vote on the
            // levels - which leaves those sitting where the last real audio put them.
            const float SILENCE_FLOOR = -200.0f;
            const bool levelsUsable = std::isfinite(minn) && std::isfinite(maxx) && maxx > SILENCE_FLOOR;
            if (levelsUsable) {
                minMaxQueue.emplace_back(minn, maxx);
            }

            int AVERAGE_SECONDS = 5;
            waterfall.pushFFT();
            inputBuffer.erase(inputBuffer.begin(), inputBuffer.begin() + fftSize);
            if (minMaxQueue.empty()) {
                // Nothing but silence so far, so there is no level to work from yet.
                continue;
            }
            int start = (int)minMaxQueue.size() - waterfallRate * AVERAGE_SECONDS;
            if (start < 0) {
                start = 0;
            }
            float bmin = 0;
            float bmax = 0;
            for (int z = start; z < minMaxQueue.size(); z++) {
                bmin += minMaxQueue[z].first;
                bmax += minMaxQueue[z].second;
            }
            bmin /= (minMaxQueue.size() - start);
            bmax /= (minMaxQueue.size() - start);
            waterfall.setWaterfallMin(bmin);
            waterfall.setWaterfallMax(bmax + 30);
            if (bmin < waterfall.getFFTMin()) {
                waterfall.setFFTMin(bmin);
            }
            else {
                waterfall.setFFTMin(waterfall.getFFTMin() + 1);
            }
            if (bmax > waterfall.getFFTMax()) {
                waterfall.setFFTMax(bmax);
            }
            else {
                waterfall.setFFTMax(waterfall.getFFTMax() - 1);
            }
            while (minMaxQueue.size() > waterfallRate * AVERAGE_SECONDS) {
                minMaxQueue.erase(minMaxQueue.begin());
            }
        }

    }
};

SubWaterfall::SubWaterfall(int sampleRate, int wfrange, const std::string & lbl) {
    pvt = std::make_shared<SubWaterfallPrivate>();
    pvt->pub = this;
    pvt->sampleRate = sampleRate;
    pvt->lbl = lbl;
    pvt->hiFreq = wfrange;
    pvt->waterfall.WATERFALL_NUMBER_OF_SECTIONS = 5;
    pvt->fftSize = pvt->hiFreq / pvt->waterfallRate;
    pvt->waterfall.setRawFFTSize(pvt->fftSize);
    // The spectrum handed to this widget spans -hiFreq..+hiFreq with DC in the
    // middle, since flushDrawUpdates swaps the FFT halves. So view the whole of it,
    // centred. It used to ask for a half width view offset by +hiFreq, which
    // setViewOffset clamps to wholeBandwidth/2 - viewBandwidth/2 - the top half of
    // the span - leaving DC against the left edge instead of the centre.
    pvt->waterfall.setBandwidth(2 * pvt->hiFreq);
    pvt->waterfall.setViewBandwidth(2 * pvt->hiFreq);
    pvt->waterfall.setViewOffset(0);
    pvt->waterfall.setFFTMin(-150);
    pvt->waterfall.setFFTMax(0);
    pvt->waterfall.setWaterfallMin(-150);
    pvt->waterfall.setWaterfallMax(0);
    pvt->waterfall.setFullWaterfallUpdate(false);
    // The whole span is always in view, so there is nothing for a drag or a scroll on
    // the scale to do except renumber it.
    pvt->waterfall.freqScaleInteractive = false;

    pvt->fft_in = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * pvt->fftSize);
    pvt->fft_out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * pvt->fftSize);
    pvt->fftwPlan = fftwf_plan_dft_1d(pvt->fftSize, pvt->fft_in, pvt->fft_out, FFTW_FORWARD, FFTW_ESTIMATE);
    pvt->spectrumLine = (float*)volk_malloc(pvt->fftSize * sizeof(float), 16);
}

SubWaterfall::~SubWaterfall() {
    fftwf_destroy_plan(pvt->fftwPlan);
    fftwf_free(pvt->fft_in);
    fftwf_free(pvt->fft_out);
    volk_free(pvt->spectrumLine);
}

void SubWaterfall::init() {
    pvt->waterfall.init();
    pvt->res.init(nullptr, pvt->sampleRate, 2 * pvt->hiFreq);
}

void SubWaterfall::draw() {
    pvt->waterfall.draw();
    pvt->flushDrawUpdates();
}

void SubWaterfall::addAudioSamples(dsp::stereo_t* samples, int count, int sampleRate) {
    // sampleRate comes from SinkManager::getStreamSampleRate, which answers -1 for a
    // stream it does not know. Dividing by that makes newSize negative, and the
    // comparison against resampledV.size() converts it to a huge size_t, so the
    // resize below asks for gigabytes and throws. Zero would divide by zero outright.
    if (samples == NULL || count <= 0 || sampleRate <= 0) { return; }
    int newSize = 1000 + (pvt->res.getOutSampleRate() * count / sampleRate);
    if (newSize < 0) { return; }
    if (pvt->resampledV.size() < newSize) {
        pvt->resampledV.resize(newSize);
    }
    if (pvt->res.getInSampleRate() != sampleRate) {
        pvt->res.setInSamplerate(sampleRate);
    }
    count = pvt->res.process(count, samples, pvt->resampledV.data());
    samples = pvt->resampledV.data();

    std::lock_guard<std::mutex> lock(pvt->inputBufferMutex);
    int curr = pvt->inputBuffer.size();
    pvt->inputBuffer.resize(curr + count);
    memcpy(pvt->inputBuffer.data() + curr, samples, count * sizeof(dsp::stereo_t));
}
void SubWaterfall::setFreqVisible(bool visible) {
    pvt->waterfall.horizontalScaleVisible = visible;
}
