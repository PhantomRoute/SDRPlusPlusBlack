#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include <gui/widgets/signal_analyzer.h>
#include <imgui.h>
#include <imgui/imgui_internal.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <utils/event.h>
#include <utils/flog.h>
#include <signal_path/signal_path.h>
#include <signal_path/symbol_tap.h>
#include <dsp/sink.h>
#include <fftw3.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>

namespace {
    // Samples kept from the channel: five seconds at 48 kHz, half a second for a wide
    // channel, so the Inst. freq view can be widened out to see slow changes. The
    // measurements only ever use the newest ANALYSIS_SAMPLES of them.
    const size_t RING_SAMPLES = 262144;
    const size_t ANALYSIS_SAMPLES = 16384;
    // Samples the symbol-rate search looks at. A power of two for the FFT.
    const int RATE_FFT = 8192;
    // A symbol-rate line has to stand this far above the spectrum around it before it is
    // reported. Noise alone puts its tallest bin about 9 dB above its neighbours, so
    // anything much lower than this could be nothing at all.
    const float MIN_LINE_DB = 14.0f;
    // Points drawn across one eye trace, which spans two symbols.
    // Points per symbol along an eye trace.
    const int EYE_POINTS_PER_SYMBOL = 24;
    // Time across the Inst. freq view, when it is not left to choose for itself.
    const double TRACE_SPANS_MS[] = { 0.2, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0 };
    const int TRACE_SPAN_COUNT = 14;
    // Symbols across the eye.
    const int EYE_SPANS[] = { 1, 2, 3, 4, 6, 8 };
    const int EYE_SPAN_COUNT = 6;
    const int MAX_EYE_TRACES = 160;
    // Decisions across the Inst. freq view for a decoder that passes only those.
    const size_t DECISIONS_AUTO = 200;
    // Bins for finding how fast the constellation turns. Over a symbol rate of 2400 and a
    // power of 4 that is 0.15 Hz a bin, which holds the groups still across the window.
    const int ROT_FFT = 4096;
    const char* CHANNEL_NAME = "_signal_analyzer";
    const double SIGAN_PI = 3.14159265358979323846;

    enum View { VIEW_FREQ = 0, VIEW_EYE, VIEW_CONST };
    enum Source { SOURCE_CHANNEL = 0, SOURCE_DECODER };
    enum EyeOf { EYE_OF_FREQ = 0, EYE_OF_I };

    // Keeps the newest samples from the analyzer's channel.
    class ChannelSink : public dsp::Sink<dsp::complex_t> {
        using base_type = dsp::Sink<dsp::complex_t>;
    public:
        ChannelSink() { ring.resize(RING_SAMPLES); }

        int run() override {
            int count = base_type::_in->read();
            if (count < 0) { return -1; }
            {
                std::lock_guard<std::mutex> lck(mtx);
                const dsp::complex_t* in = base_type::_in->readBuf;
                for (int i = 0; i < count; i++) {
                    ring[pos] = in[i];
                    pos = (pos + 1) % RING_SAMPLES;
                }
                filled = std::min<size_t>(RING_SAMPLES, filled + (size_t)count);
            }
            base_type::_in->flush();
            return count;
        }

        size_t copyNewest(std::vector<dsp::complex_t>& out, size_t maxCount) {
            std::lock_guard<std::mutex> lck(mtx);
            size_t n = std::min<size_t>(filled, maxCount);
            out.resize(n);
            for (size_t i = 0; i < n; i++) {
                out[i] = ring[(pos + RING_SAMPLES - n + i) % RING_SAMPLES];
            }
            return n;
        }

        void clear() {
            std::lock_guard<std::mutex> lck(mtx);
            filled = 0;
            pos = 0;
        }

    private:
        std::mutex mtx;
        std::vector<dsp::complex_t> ring;
        size_t pos = 0;
        size_t filled = 0;
    };

    // ---- Measurement helpers. Everything below works on plain series and is shared by
    // the channel and decoder sources, so the two are measured the same way.

    float interpAt(const std::vector<float>& y, double t) {
        if (y.empty()) { return 0.0f; }
        if (t <= 0.0) { return y.front(); }
        size_t last = y.size() - 1;
        if (t >= (double)last) { return y.back(); }
        size_t i = (size_t)t;
        float fr = (float)(t - (double)i);
        return y[i] + ((y[i + 1] - y[i]) * fr);
    }

    float percentileOf(std::vector<float> v, double p) {
        if (v.empty()) { return 0.0f; }
        size_t k = (size_t)std::min<double>((double)(v.size() - 1), std::max<double>(0.0, p * (double)(v.size() - 1)));
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    }

    // The distinct values a series keeps returning to, from a histogram of it. A level
    // is a histogram peak at least 15% as tall as the tallest, and two peaks with no
    // real dip between them count as one. More than eight is not a set of levels.
    std::vector<float> findLevels(const std::vector<float>& v) {
        std::vector<float> levels;
        if (v.size() < 40) { return levels; }
        float lo = percentileOf(v, 0.01);
        float hi = percentileOf(v, 0.99);
        if (!(hi > lo)) { return levels; }
        float span = hi - lo;
        lo -= span * 0.1f;
        hi += span * 0.1f;
        const int B = 96;
        std::vector<float> raw(B, 0.0f), hist(B, 0.0f);
        float binW = (hi - lo) / (float)B;
        for (float x : v) {
            int b = (int)((x - lo) / binW);
            if (b >= 0 && b < B) { raw[b] += 1.0f; }
        }
        const float kernel[5] = { 1.0f, 2.0f, 3.0f, 2.0f, 1.0f };
        for (int i = 0; i < B; i++) {
            float s = 0.0f;
            for (int k = -2; k <= 2; k++) {
                int j = i + k;
                if (j >= 0 && j < B) { s += raw[j] * kernel[k + 2]; }
            }
            hist[i] = s / 9.0f;
        }
        float maxH = *std::max_element(hist.begin(), hist.end());
        if (maxH <= 0.0f) { return levels; }

        std::vector<int> peaks;
        for (int i = 0; i < B; i++) {
            float left = (i > 0) ? hist[i - 1] : 0.0f;
            float right = (i < B - 1) ? hist[i + 1] : 0.0f;
            if (hist[i] > left && hist[i] >= right && hist[i] >= maxH * 0.15f) { peaks.push_back(i); }
        }
        // Merge neighbours without a real valley between them.
        bool merged = true;
        while (merged && peaks.size() > 1) {
            merged = false;
            for (size_t p = 0; p + 1 < peaks.size(); p++) {
                int a = peaks[p], b = peaks[p + 1];
                float valley = *std::min_element(hist.begin() + a, hist.begin() + b + 1);
                if (valley > 0.6f * std::min<float>(hist[a], hist[b])) {
                    if (hist[a] >= hist[b]) { peaks.erase(peaks.begin() + p + 1); }
                    else { peaks.erase(peaks.begin() + p); }
                    merged = true;
                    break;
                }
            }
        }
        if (peaks.size() > 8) { return levels; }
        for (int pk : peaks) {
            float sw = 0.0f, sx = 0.0f;
            for (int k = -3; k <= 3; k++) {
                int j = pk + k;
                if (j < 0 || j >= B) { continue; }
                sw += raw[j];
                sx += raw[j] * (lo + ((float)j + 0.5f) * binW);
            }
            levels.push_back(sw > 0.0f ? sx / sw : lo + ((float)pk + 0.5f) * binW);
        }
        return levels;
    }

    int nearestLevel(const std::vector<float>& levels, float x) {
        int best = 0;
        float bestD = 1e30f;
        for (int i = 0; i < (int)levels.size(); i++) {
            float d = fabsf(x - levels[i]);
            if (d < bestD) { bestD = d; best = i; }
        }
        return best;
    }

    // How open the eye is, as the narrowest gap between the spread of one level and the
    // spread of the next, over the spacing between them. The 5th and 95th percentiles,
    // so a stray symbol does not shut it. False without two levels to measure between.
    bool eyeOpeningOf(const std::vector<float>& values, const std::vector<float>& levels, float& opening) {
        if (levels.size() < 2 || values.size() < 40) { return false; }
        std::vector<std::vector<float>> clusters(levels.size());
        for (float v : values) { clusters[nearestLevel(levels, v)].push_back(v); }
        float worst = 1.0f;
        bool any = false;
        for (size_t j = 0; j + 1 < levels.size(); j++) {
            if (clusters[j].size() < 5 || clusters[j + 1].size() < 5) { continue; }
            float spacing = levels[j + 1] - levels[j];
            if (spacing <= 0.0f) { continue; }
            float gap = percentileOf(clusters[j + 1], 0.05) - percentileOf(clusters[j], 0.95);
            worst = std::min<float>(worst, gap / spacing);
            any = true;
        }
        if (!any) { return false; }
        opening = std::clamp<float>(worst, 0.0f, 1.0f);
        return true;
    }

    // How long a change from one level to another takes, from 20% of the way to 80%,
    // as a fraction of a symbol, averaged over every change in the window.
    bool edgeFractionOf(const std::vector<float>& y, const std::vector<double>& instants, double T,
                        const std::vector<float>& levels, float& fraction) {
        if (levels.size() < 2 || instants.size() < 20 || T <= 1.0) { return false; }
        double sum = 0.0;
        int count = 0;
        const int STEPS = 48;
        for (size_t k = 0; k + 1 < instants.size() && count < 600; k++) {
            int a = nearestLevel(levels, interpAt(y, instants[k]));
            int b = nearestLevel(levels, interpAt(y, instants[k + 1]));
            if (a == b) { continue; }
            float from = levels[a], to = levels[b];
            float at20 = from + ((to - from) * 0.2f);
            float at80 = from + ((to - from) * 0.8f);
            double t20 = -1.0, t80 = -1.0;
            double span = instants[k + 1] - instants[k];
            for (int s = 0; s <= STEPS; s++) {
                double t = instants[k] + (span * (double)s / (double)STEPS);
                float v = interpAt(y, t);
                bool past20 = (to > from) ? (v >= at20) : (v <= at20);
                bool past80 = (to > from) ? (v >= at80) : (v <= at80);
                if (t20 < 0.0 && past20) { t20 = t; }
                if (t80 < 0.0 && past80) { t80 = t; break; }
            }
            if (t20 < 0.0 || t80 < t20) { continue; }
            sum += (t80 - t20) / T;
            count++;
        }
        if (count < 10) { return false; }
        fraction = (float)(sum / (double)count);
        return true;
    }

    // Counts distinct groups in a set of angles, the same way levels are counted but
    // wrapping round: histogram peaks at least 30% as tall as the tallest, with peaks that
    // have no real dip between them counted once. Zero when there is no clear grouping.
    int angleGroupsOf(const std::vector<float>& angles) {
        if (angles.size() < 200) { return 0; }
        const int B = 72;
        std::vector<float> raw(B, 0.0f), hist(B, 0.0f);
        for (float a : angles) {
            int b = (int)floor((a + SIGAN_PI) / (2.0 * SIGAN_PI) * B);
            b = ((b % B) + B) % B;
            raw[b] += 1.0f;
        }
        const float kernel[5] = { 1.0f, 2.0f, 3.0f, 2.0f, 1.0f };
        for (int i = 0; i < B; i++) {
            float sum = 0.0f;
            for (int k = -2; k <= 2; k++) { sum += raw[(i + k + B) % B] * kernel[k + 2]; }
            hist[i] = sum / 9.0f;
        }
        float maxH = *std::max_element(hist.begin(), hist.end());
        float minH = *std::min_element(hist.begin(), hist.end());
        // A flat ring has no groups to count.
        if (maxH <= 0.0f || minH > maxH * 0.5f) { return 0; }
        std::vector<int> peaks;
        for (int i = 0; i < B; i++) {
            if (hist[i] > hist[(i + B - 1) % B] && hist[i] >= hist[(i + 1) % B] && hist[i] >= maxH * 0.3f) { peaks.push_back(i); }
        }
        bool merged = true;
        while (merged && peaks.size() > 1) {
            merged = false;
            for (size_t p = 0; p < peaks.size(); p++) {
                int a = peaks[p];
                int b = peaks[(p + 1) % peaks.size()];
                int len = ((b - a) + B) % B;
                if (len == 0) { len = B; }
                float valley = hist[a];
                for (int k = 0; k <= len; k++) { valley = std::min<float>(valley, hist[(a + k) % B]); }
                if (valley > 0.6f * std::min<float>(hist[a], hist[b])) {
                    peaks.erase(peaks.begin() + ((hist[a] >= hist[b]) ? (int)((p + 1) % peaks.size()) : (int)p));
                    merged = true;
                    break;
                }
            }
        }
        if (peaks.size() < 2 || peaks.size() > 16) { return 0; }
        return (int)peaks.size();
    }

    // Everything the channel views draw, kept apart from the measurements so Pause can
    // hold the picture still while Signal ID's numbers carry on.
    struct ChannelView {
        bool ready = false;
        sigan::Measurements meas;
        std::vector<float> freq;              // Hz, the analysis window
        float freqRange = 1000.0f;            // its scale, which the eye uses
        std::vector<double> instants;         // symbol instants in freq
        std::vector<float> levels;
        std::vector<float> trace;             // Hz, however much time the Inst. freq view shows
        std::vector<double> traceMarks;       // symbol instants in trace
        float traceRange = 1000.0f;
        std::vector<float> iPart;
        std::vector<double> constInstants;
        std::vector<dsp::complex_t> constPoints;
        std::vector<float> constAngles;
        int constGroups = 0;
        float constSpread = 0.0f;
        bool rotFound = false;
        double rotHz = 0.0;
        double T = 0.0;
        double sampleRate = 48000.0;
    };

    class Analyzer {
    public:
        bool shown = false;
        bool paused = false;
        int traceSpanIdx = -1;     // -1 lets the view choose: about forty symbols
        int eyeSpanIdx = 1;        // two symbols
        int view = VIEW_FREQ;
        int source = SOURCE_CHANNEL;
        int eyeOf = EYE_OF_FREQ;
        double lastRequest = -1000.0;

        sigan::Measurements meas;

        Analyzer() {
            fftIn = (fftwf_complex*)fftwf_malloc(RATE_FFT * sizeof(fftwf_complex));
            fftOut = (fftwf_complex*)fftwf_malloc(RATE_FFT * sizeof(fftwf_complex));
            fftPlan = fftwf_plan_dft_1d(RATE_FFT, fftIn, fftOut, FFTW_FORWARD, FFTW_ESTIMATE);
            rotIn = (fftwf_complex*)fftwf_malloc(ROT_FFT * sizeof(fftwf_complex));
            rotOut = (fftwf_complex*)fftwf_malloc(ROT_FFT * sizeof(fftwf_complex));
            rotPlan = fftwf_plan_dft_1d(ROT_FFT, rotIn, rotOut, FFTW_FORWARD, FFTW_ESTIMATE);
        }

        void tick() {
            double now = ImGui::GetTime();
            bool running = gui::mainWindow.sdrIsRunning();
            bool wantChannel = running && ((shown && source == SOURCE_CHANNEL) || (now - lastRequest) < 1.5);
            wantChannel = wantChannel && selectedVfo(selName, selOffset, selBandwidth);

            if (shown) { showWindow(); }
            else { hideWindow(); }

            bool wantSymbols = running && shown && source == SOURCE_DECODER;
            if (wantSymbols != symbolsWanted) {
                symbolsWanted = wantSymbols;
                symboltap::setWanted(wantSymbols);
            }

            if (!wantChannel) {
                closeChannel();
                meas = sigan::Measurements();
            }
            else {
                followVfo();
            }

            if ((now - lastAnalysis) < 0.25) { return; }
            lastAnalysis = now;
            if (channel != nullptr) {
                // The measurements carry on whatever is on screen.
                size_t want = std::max<size_t>(ANALYSIS_SAMPLES, traceSamplesWanted(chanRate, cv.T));
                sink.copyNewest(iqLong, std::min<size_t>(RING_SAMPLES, want));
                analyzeChannel(iqLong, chanRate, paused ? cvScratch : cv, meas);
                if (paused) {
                    // Paused, the plot is drawn from a copy of everything the channel held
                    // at the time, so the span can still be changed to look closer or
                    // further out at that moment.
                    if (frozenIq.empty() || !cv.ready) { freezeChannel(); }
                    else if (frozenSpanIdx != traceSpanIdx) {
                        sigan::Measurements unused;
                        analyzeChannel(frozenIq, frozenRate, cv, unused);
                        frozenSpanIdx = traceSpanIdx;
                    }
                }
            }
            if (shown && source == SOURCE_DECODER) {
                // Paused, a new snapshot is only taken when there is nothing to hold yet.
                if (!paused || !decReady) { analyzeDecoder(true); }
                else if (decSpanIdx != traceSpanIdx) { analyzeDecoder(false); }
            }
        }

        void setPaused(bool p) {
            paused = p;
            frozenIq.clear();
            if (p && channel != nullptr) { freezeChannel(); }
        }

        void draw();

    private:
        // ---- The channel.

        bool selectedVfo(std::string& name, double& offset, double& bandwidth) {
            name = gui::waterfall.selectedVFO;
            if (name.empty()) { return false; }
            auto it = gui::waterfall.vfos.find(name);
            if (it == gui::waterfall.vfos.end() || it->second == nullptr) { return false; }
            offset = it->second->centerOffset;
            bandwidth = it->second->bandwidth;
            return bandwidth > 0.0;
        }

        // Wide enough to take the whole channel with some room either side, never below
        // 48 kHz so narrow signals still have a good number of samples per symbol, and no
        // more than the front end delivers.
        double rateFor(double bandwidth) {
            double want = std::max<double>(48000.0, ceil(bandwidth * 2.5 / 1000.0) * 1000.0);
            double available = sigpath::iqFrontEnd.getEffectiveSamplerate();
            if (available > 0.0) { want = std::min<double>(want, available); }
            return want;
        }

        void followVfo() {
            double rate = rateFor(selBandwidth);
            if (channel == nullptr) {
                channel = sigpath::iqFrontEnd.addVFO(CHANNEL_NAME, rate, selBandwidth, selOffset);
                if (channel == nullptr) { return; }
                if (!sinkInit) {
                    sink.init(&channel->out);
                    sinkInit = true;
                }
                else {
                    sink.setInput(&channel->out);
                }
                sink.clear();
                sink.start();
                chanRate = rate;
                chanBandwidth = selBandwidth;
                chanOffset = selOffset;
                return;
            }
            if (rate != chanRate || selBandwidth != chanBandwidth) {
                channel->setOutSamplerate(rate, selBandwidth);
                chanRate = rate;
                chanBandwidth = selBandwidth;
                sink.clear();
            }
            if (selOffset != chanOffset) {
                channel->setOffset(selOffset);
                chanOffset = selOffset;
                // Samples from before a retune describe a different piece of spectrum.
                sink.clear();
            }
        }

        void closeChannel() {
            if (channel == nullptr) { return; }
            sink.stop();
            sigpath::iqFrontEnd.removeVFO(CHANNEL_NAME);
            channel = nullptr;
            sink.clear();
            cv.ready = false;
            cvScratch.ready = false;
            frozenIq.clear();
        }

        // Copies everything the channel holds and draws from it until Pause is released.
        void freezeChannel() {
            sink.copyNewest(frozenIq, RING_SAMPLES);
            frozenRate = chanRate;
            frozenSpanIdx = traceSpanIdx;
            sigan::Measurements unused;
            analyzeChannel(frozenIq, frozenRate, cv, unused);
            if (!cv.ready) { frozenIq.clear(); }
        }

        void showWindow() {
            if (gui::mainWindow.hasBottomWindow("signal_analyzer")) { return; }
            gui::mainWindow.addBottomWindow("signal_analyzer", [this]() { draw(); });
        }

        void hideWindow() {
            if (!gui::mainWindow.hasBottomWindow("signal_analyzer")) { return; }
            gui::mainWindow.removeBottomWindow("signal_analyzer");
        }

        // The clearest line in the spectrum of a series, in hertz, between 100 Hz and a
        // little under half the sample rate, and how far it stands above the spectrum
        // around it.
        //
        // Around it, not above the whole spectrum's median. The features this is run on
        // are far from flat - how much the frequency jumps from one sample to the next has
        // most of its energy low down - and against the median of everything a broad
        // hump near 100 Hz read as a line tens of dB tall, which on a QPSK signal gave a
        // symbol rate of 200 or 300 Bd for one sending 2400.
        bool lineOf(const std::vector<float>& feature, double sr, double& freq, float& lineDb) {
            if ((int)feature.size() < RATE_FFT) { return false; }
            size_t start = feature.size() - RATE_FFT;
            double mean = 0.0;
            for (int i = 0; i < RATE_FFT; i++) { mean += feature[start + i]; }
            mean /= RATE_FFT;
            for (int i = 0; i < RATE_FFT; i++) {
                double w = 0.5 - (0.5 * cos((2.0 * SIGAN_PI * i) / (RATE_FFT - 1)));
                fftIn[i][0] = (float)((feature[start + i] - mean) * w);
                fftIn[i][1] = 0.0f;
            }
            fftwf_execute(fftPlan);

            const int HALF = RATE_FFT / 2;
            powerScratch.resize(HALF);
            prefix.resize(HALF + 1);
            prefix[0] = 0.0;
            for (int k = 0; k < HALF; k++) {
                powerScratch[k] = (fftOut[k][0] * fftOut[k][0]) + (fftOut[k][1] * fftOut[k][1]);
                prefix[k + 1] = prefix[k] + powerScratch[k];
            }
            // The neighbourhood: 48 bins either side, leaving out the 4 nearest, which the
            // window spreads the line itself into.
            const int LINE_NEAR = 4, LINE_FAR = 48;
            auto ratioAt = [&](int k) -> double {
                int aLo = std::max<int>(1, k - LINE_FAR), aHi = k - LINE_NEAR;
                int bLo = k + LINE_NEAR, bHi = std::min<int>(HALF - 1, k + LINE_FAR);
                double sum = 0.0;
                int cnt = 0;
                if (aHi >= aLo) { sum += prefix[aHi + 1] - prefix[aLo]; cnt += aHi - aLo + 1; }
                if (bHi >= bLo) { sum += prefix[bHi + 1] - prefix[bLo]; cnt += bHi - bLo + 1; }
                if (cnt < 16 || sum <= 0.0) { return 0.0; }
                return (double)powerScratch[k] / (sum / cnt);
            };

            int kmin = std::max<int>(LINE_NEAR + 2, (int)ceil(100.0 * RATE_FFT / sr));
            int kmax = std::min<int>(HALF - LINE_NEAR - 2, (int)floor((sr / 2.5) * RATE_FFT / sr));
            if (kmax <= kmin + 8) { return false; }
            int bestK = -1;
            double bestR = 0.0;
            for (int k = kmin; k <= kmax; k++) {
                if (powerScratch[k] < powerScratch[k - 1] || powerScratch[k] < powerScratch[k + 1]) { continue; }
                double r = ratioAt(k);
                if (r > bestR) { bestR = r; bestK = k; }
            }
            if (bestK < 0) { return false; }

            // A harmonic can outdo the line it is a harmonic of. A line at half the
            // frequency within 6 dB of it is the one that actually repeats.
            int halfK = bestK / 2;
            if (halfK - 2 >= kmin) {
                int hk = -1;
                double hr = 0.0;
                for (int k = halfK - 2; k <= halfK + 2; k++) {
                    double r = ratioAt(k);
                    if (r > hr) { hr = r; hk = k; }
                }
                if (hk >= 0 && hr >= bestR * 0.25) { bestR = hr; bestK = hk; }
            }

            lineDb = (float)(10.0 * log10(std::max<double>(bestR, 1e-12)));
            auto magAt = [&](int k) { return sqrt((double)powerScratch[k]); };
            double a = magAt(bestK - 1), b = magAt(bestK), c = magAt(bestK + 1);
            double denom = a - (2.0 * b) + c;
            double delta = (denom < 0.0) ? std::clamp<double>(0.5 * (a - c) / denom, -0.5, 0.5) : 0.0;
            freq = ((double)bestK + delta) * sr / RATE_FFT;
            return true;
        }

        // How many samples the Inst. freq view wants, given how many symbols or
        // milliseconds it has been asked for.
        size_t traceSamplesWanted(double sr, double T) {
            if (traceSpanIdx < 0) {
                return (T >= 2.0) ? (size_t)(40.0 * T) : 2048;
            }
            return (size_t)(TRACE_SPANS_MS[traceSpanIdx] * sr / 1000.0);
        }

        // Whether the decoder passes a waveform with its decisions marked on it, rather
        // than only the decisions.
        bool decWaveform() const { return snap.oversampled && decT >= 2.0; }

        // The time the Inst. freq view covers when left to choose, in milliseconds.
        double autoTraceMs() {
            if (source == SOURCE_DECODER && decReady && !decWaveform()) {
                return (snap.symbolRate > 0.0) ? 1000.0 * (double)DECISIONS_AUTO / snap.symbolRate : 0.0;
            }
            double sr = (source == SOURCE_CHANNEL) ? cv.sampleRate : snap.sampleRate;
            double T = (source == SOURCE_CHANNEL) ? cv.T : decT;
            return (sr > 0.0) ? 1000.0 * (double)traceSamplesWanted(sr, T) / sr : 0.0;
        }

        // Measures the newest ANALYSIS_SAMPLES of src and fills out from it. src is one
        // copy covering both the measurements and however far back the Inst. freq view
        // reaches, so the symbol marks line up with the trace.
        void analyzeChannel(const std::vector<dsp::complex_t>& src, double sr, ChannelView& out, sigan::Measurements& meas) {
            size_t total = src.size();
            size_t n = std::min<size_t>(total, ANALYSIS_SAMPLES);
            iq.assign(src.end() - n, src.end());
            if (n < (size_t)RATE_FFT + 16) {
                // Just after a retune: nothing yet, and nothing left over from before it.
                meas = sigan::Measurements();
                meas.running = true;
                out.ready = false;
                return;
            }
            meas.running = true;
            out.sampleRate = sr;

            // Instantaneous frequency, in hertz from the channel centre.
            out.freq.resize(n);
            out.freq[0] = 0.0f;
            double wsum = 0.0, fsum = 0.0, magSum = 0.0;
            for (size_t i = 1; i < n; i++) {
                float re = (iq[i].re * iq[i - 1].re) + (iq[i].im * iq[i - 1].im);
                float im = (iq[i].im * iq[i - 1].re) - (iq[i].re * iq[i - 1].im);
                out.freq[i] = (float)(atan2(im, re) * sr / (2.0 * SIGAN_PI));
                double w = (iq[i].re * iq[i].re) + (iq[i].im * iq[i].im);
                wsum += w;
                fsum += w * out.freq[i];
                magSum += sqrt(w);
            }
            out.freq[0] = out.freq[1];
            meas.haveOffset = wsum > 0.0;
            meas.offsetHz = (wsum > 0.0) ? fsum / wsum : 0.0;
            double rmsMag = sqrt(wsum / (double)n);
            (void)magSum;

            // The symbol rate: frequency changes for a signal that moves in frequency,
            // envelope changes for one that moves in amplitude. Whichever line is clearer.
            featF.resize(n);
            featA.resize(n);
            featF[0] = 0.0f;
            featA[0] = 0.0f;
            float prevMag = sqrtf((iq[0].re * iq[0].re) + (iq[0].im * iq[0].im));
            for (size_t i = 1; i < n; i++) {
                featF[i] = fabsf(out.freq[i] - out.freq[i - 1]);
                float mag = sqrtf((iq[i].re * iq[i].re) + (iq[i].im * iq[i].im));
                featA[i] = fabsf(mag - prevMag);
                prevMag = mag;
            }
            double rateF = 0.0, rateA = 0.0;
            float dbF = 0.0f, dbA = 0.0f;
            bool okF = lineOf(featF, sr, rateF, dbF);
            bool okA = lineOf(featA, sr, rateA, dbA);
            meas.haveSymbolRate = false;
            if (okF || okA) {
                bool useF = okF && (!okA || dbF >= dbA);
                // The two can land on different lines of the same signal. The phase
                // jumps of a PSK signal put their clearest line at twice the rate its
                // envelope repeats at; when the other one found a clear line at half
                // this one, that is the rate.
                double chosen = useF ? rateF : rateA;
                double other = useF ? rateA : rateF;
                float otherDb = useF ? dbA : dbF;
                bool otherOk = useF ? okA : okF;
                if (otherOk && otherDb >= MIN_LINE_DB && other > 0.0 && fabs((other * 2.0) - chosen) < chosen * 0.02) {
                    useF = !useF;
                }
                float db = useF ? dbF : dbA;
                if (db >= MIN_LINE_DB) {
                    meas.haveSymbolRate = true;
                    meas.symbolRate = useF ? rateF : rateA;
                    meas.lineDb = db;
                }
            }

            meas.levels = 0;
            meas.haveEye = false;
            meas.haveEdges = false;
            out.instants.clear();
            out.levels.clear();
            double T = meas.haveSymbolRate ? sr / meas.symbolRate : 0.0;

            if (meas.haveSymbolRate && T >= 2.0) {
                int K = (int)floor(((double)n - (3.0 * T)) / T);
                if (K >= 40) {
                    // The sampling point: first the one where the frequency spreads out
                    // the most, which is the middle of the symbols; then, once the levels
                    // are known, the one where the eye is widest.
                    const int PHASES = 24;
                    int bestP = 0;
                    double bestVar = -1.0;
                    for (int p = 0; p < PHASES; p++) {
                        double t0 = T + (T * p / PHASES);
                        double s = 0.0, s2 = 0.0;
                        for (int k = 0; k < K; k++) {
                            double v = interpAt(out.freq, t0 + (k * T));
                            s += v;
                            s2 += v * v;
                        }
                        double var = (s2 / K) - ((s / K) * (s / K));
                        if (var > bestVar) { bestVar = var; bestP = p; }
                    }
                    std::vector<float> values(K);
                    auto sampleAt = [&](int p) {
                        double t0 = T + (T * p / PHASES);
                        for (int k = 0; k < K; k++) { values[k] = interpAt(out.freq, t0 + (k * T)); }
                    };
                    sampleAt(bestP);
                    std::vector<float> levels = findLevels(values);
                    if (levels.size() >= 2) {
                        float bestOpen = -1.0f;
                        int openP = bestP;
                        for (int p = 0; p < PHASES; p++) {
                            sampleAt(p);
                            float o = 0.0f;
                            if (eyeOpeningOf(values, levels, o) && o > bestOpen) { bestOpen = o; openP = p; }
                        }
                        bestP = openP;
                        sampleAt(bestP);
                        levels = findLevels(values);
                    }
                    double t0 = T + (T * bestP / PHASES);
                    for (int k = 0; k < K; k++) { out.instants.push_back(t0 + (k * T)); }

                    if (!levels.empty()) {
                        meas.levels = (int)std::min<size_t>(levels.size(), 8);
                        for (int i = 0; i < meas.levels; i++) { meas.levelHz[i] = levels[i]; }
                        meas.deviationHz = (levels.back() - levels.front()) / 2.0f;
                        out.levels = levels;
                        float o = 0.0f;
                        if (eyeOpeningOf(values, levels, o)) { meas.haveEye = true; meas.eyeOpening = o; }
                        float e = 0.0f;
                        if (edgeFractionOf(out.freq, out.instants, T, levels, e)) { meas.haveEdges = true; meas.edgeFraction = e; }
                    }
                }
            }

            // ---- What the views draw.

            // Frequency over however much time the view asks for, from the long copy.
            size_t traceLen = std::min<size_t>(total, std::max<size_t>(8, traceSamplesWanted(sr, T)));
            size_t traceFrom = total - traceLen;
            out.trace.resize(traceLen);
            for (size_t j = 0; j < traceLen; j++) {
                size_t i = traceFrom + j;
                if (i == 0) { out.trace[j] = 0.0f; continue; }
                float re = (src[i].re * src[i - 1].re) + (src[i].im * src[i - 1].im);
                float im = (src[i].im * src[i - 1].re) - (src[i].re * src[i - 1].im);
                out.trace[j] = (float)(atan2(im, re) * sr / (2.0 * SIGAN_PI));
            }
            if (traceLen > 1 && traceFrom == 0) { out.trace[0] = out.trace[1]; }
            // The analysis window is the newest n samples of the long copy.
            double windowStart = (double)(total - n);
            out.traceMarks.clear();
            for (double t : out.instants) {
                double at = t + windowStart - (double)traceFrom;
                if (at >= 0.0) { out.traceMarks.push_back(at); }
            }
            // Each plot scaled to what it shows. The eye's from the analysis window, so
            // narrowing the Inst. freq span to a few samples does not squash the eye.
            auto rangeOf = [&](const std::vector<float>& v) {
                float r = std::max<float>(percentileOf(v, 0.99), -percentileOf(v, 0.01));
                if (!out.levels.empty()) {
                    r = std::max<float>(r, std::max<float>(fabsf(out.levels.front()), fabsf(out.levels.back())));
                }
                return std::max<float>(r * 1.15f, 100.0f);
            };
            out.traceRange = rangeOf(out.trace);
            out.freqRange = rangeOf(out.freq);

            // Points for the I eye and the constellation, normalised to the average magnitude.
            out.iPart.resize(n);
            float norm = (rmsMag > 0.0) ? (float)(1.0 / rmsMag) : 1.0f;
            for (size_t i = 0; i < n; i++) { out.iPart[i] = iq[i].re * norm; }

            // The constellation's sampling point is chosen separately from the frequency
            // one: where the magnitude varies least from symbol to symbol, which is the
            // symbol instant for a signal that carries its information in phase.
            out.constPoints.clear();
            out.constAngles.clear();
            if (T >= 2.0) {
                int K = (int)floor(((double)n - (3.0 * T)) / T);
                const int PHASES = 24;
                int bestP = 0;
                double bestCv = 1e30;
                for (int p = 0; p < PHASES && K >= 40; p++) {
                    double t0 = T + (T * p / PHASES);
                    double s = 0.0, s2 = 0.0;
                    for (int k = 0; k < K; k++) {
                        size_t i = (size_t)(t0 + (k * T));
                        double m = sqrt((iq[i].re * iq[i].re) + (iq[i].im * iq[i].im));
                        s += m;
                        s2 += m * m;
                    }
                    double mean = s / K;
                    double cv = (mean > 0.0) ? sqrt(std::max<double>(0.0, (s2 / K) - (mean * mean))) / mean : 1e30;
                    if (cv < bestCv) { bestCv = cv; bestP = p; }
                }
                double t0 = T + (T * bestP / PHASES);
                out.constSpread = (float)bestCv;

                // How fast the points turn. The average frequency is only good to some
                // tens of hertz, and over a third of a second that still spins them into a
                // ring. Raising each point to a power cancels a phase that takes a fixed
                // set of values, leaving only the turning, as one clean line: the 2nd, 4th
                // and 8th powers are tried, and the clearest line is used. Only the plot is
                // turned back by it; nothing is read into which power won.
                out.rotFound = false;
                out.rotHz = 0.0;
                int usable = std::min<int>(K, ROT_FFT);
                double bestStrength = 0.0;
                for (int power : { 2, 4, 8 }) {
                    if (usable < 64) { break; }
                    for (int j = 0; j < ROT_FFT; j++) { rotIn[j][0] = 0.0f; rotIn[j][1] = 0.0f; }
                    for (int k = 0; k < usable; k++) {
                        size_t i = (size_t)(t0 + ((K - usable + k) * T));
                        double mag = sqrt((iq[i].re * iq[i].re) + (iq[i].im * iq[i].im));
                        if (mag <= 0.0) { continue; }
                        double ph = atan2(iq[i].im, iq[i].re) * power;
                        rotIn[k][0] = (float)cos(ph);
                        rotIn[k][1] = (float)sin(ph);
                    }
                    fftwf_execute(rotPlan);
                    int pk = 0;
                    double pkP = 0.0, sumP = 0.0;
                    for (int j = 0; j < ROT_FFT; j++) {
                        double pw = (rotOut[j][0] * rotOut[j][0]) + (rotOut[j][1] * rotOut[j][1]);
                        sumP += pw;
                        if (pw > pkP) { pkP = pw; pk = j; }
                    }
                    double strength = (sumP > 0.0) ? pkP / (sumP / ROT_FFT) : 0.0;
                    // Clearly above the rest: a random phase gives a peak a few times the mean.
                    if (strength > 50.0 && strength > bestStrength) {
                        bestStrength = strength;
                        double cyclesPerSymbol = (double)pk / ROT_FFT;
                        if (cyclesPerSymbol > 0.5) { cyclesPerSymbol -= 1.0; }
                        out.rotHz = cyclesPerSymbol * (sr / T) / power;
                        out.rotFound = true;
                    }
                }
                double turnHz = out.rotFound ? out.rotHz : 0.0;

                for (int k = 0; k < K; k++) {
                    double t = t0 + (k * T);
                    size_t i = (size_t)t;
                    double ang = -2.0 * SIGAN_PI * turnHz * (double)i / sr;
                    float c = (float)cos(ang), s = (float)sin(ang);
                    float re = iq[i].re * norm, im = iq[i].im * norm;
                    dsp::complex_t pt;
                    pt.re = (re * c) - (im * s);
                    pt.im = (re * s) + (im * c);
                    if (k >= K - 1200) { out.constPoints.push_back(pt); }
                    if ((pt.re * pt.re) + (pt.im * pt.im) > 0.09f) { out.constAngles.push_back(atan2f(pt.im, pt.re)); }
                }
                out.constGroups = angleGroupsOf(out.constAngles);
                out.constInstants.clear();
                for (int k = 0; k < K; k++) { out.constInstants.push_back(t0 + (k * T)); }
            }
            else {
                // No rate: every sample, which is still the shape of the signal.
                for (size_t i = (n > 1200 ? n - 1200 : 0); i < n; i++) {
                    dsp::complex_t pt;
                    pt.re = iq[i].re * norm;
                    pt.im = iq[i].im * norm;
                    out.constPoints.push_back(pt);
                }
                out.constGroups = 0;
                out.constSpread = 0.0f;
                out.constInstants.clear();
            }
            out.T = T;
            out.meas = meas;
            out.ready = true;
        }

        // ---- A decoder's own samples.

        // fresh takes a new snapshot; otherwise the one held is worked through again, which
        // is how the span can still be changed while paused.
        void analyzeDecoder(bool fresh) {
            if (fresh) {
                decReady = false;
                if (!symboltap::snapshot(snap) || snap.age > 2.0 || snap.values.size() < 40) { return; }
            }
            else if (!decReady) { return; }
            decSpanIdx = traceSpanIdx;
            decLevels = findLevels(snap.values);
            decHaveEye = eyeOpeningOf(snap.values, decLevels, decOpening);
            decT = (snap.symbolRate > 0.0) ? snap.sampleRate / snap.symbolRate : 0.0;

            // Where the eye is widest, as a fraction of a symbol either side of the point
            // the decoder sampled at.
            decHaveWidest = false;
            decHaveEdges = false;
            if (snap.oversampled && decLevels.size() >= 2 && decT >= 2.0) {
                std::vector<float> shifted(snap.marks.size());
                float bestOpen = -1.0f;
                double bestD = 0.0;
                for (int s = -10; s <= 10; s++) {
                    double d = decT * (double)s / 20.0;
                    for (size_t k = 0; k < snap.marks.size(); k++) { shifted[k] = interpAt(snap.samples, snap.marks[k] + d); }
                    float o = 0.0f;
                    if (eyeOpeningOf(shifted, decLevels, o) && (o > bestOpen + 1e-4f || (fabsf(o - bestOpen) <= 1e-4f && fabs(d) < fabs(bestD)))) {
                        bestOpen = o;
                        bestD = d;
                    }
                }
                if (bestOpen >= 0.0f) {
                    decHaveWidest = true;
                    decWidest = (float)(bestD / decT);
                }
                decHaveEdges = edgeFractionOf(snap.samples, snap.marks, decT, decLevels, decEdges);
            }

            // Share of decisions taken close to one of the decoder's thresholds: within a
            // tenth of the level spacing of it.
            decNearThreshold = -1.0f;
            float spacing = 0.0f;
            if (decLevels.size() >= 2) { spacing = (decLevels.back() - decLevels.front()) / (float)(decLevels.size() - 1); }
            else { spacing = fabsf(snap.thresholds[2] - snap.thresholds[0]) / 2.0f; }
            if (spacing > 0.0f) {
                int nearCount = 0;
                for (float v : snap.values) {
                    for (int t = 0; t < 3; t++) {
                        if (fabsf(v - snap.thresholds[t]) < spacing * 0.1f) { nearCount++; break; }
                    }
                }
                decNearThreshold = (float)nearCount / (float)snap.values.size();
            }

            // Trace: waveform, or one decision after another, over the span asked for.
            decTrace.clear();
            decTraceMarks.clear();
            if (decWaveform()) {
                size_t len = std::min<size_t>(snap.samples.size(), std::max<size_t>(8, traceSamplesWanted(snap.sampleRate, decT)));
                size_t start = snap.samples.size() - len;
                decTrace.assign(snap.samples.begin() + start, snap.samples.end());
                for (double m : snap.marks) {
                    if (m >= (double)start) { decTraceMarks.push_back(m - (double)start); }
                }
            }
            else {
                size_t wantLen = (traceSpanIdx < 0) ? DECISIONS_AUTO : (size_t)(TRACE_SPANS_MS[traceSpanIdx] * snap.symbolRate / 1000.0);
                size_t len = std::min<size_t>(snap.values.size(), std::max<size_t>(8, wantLen));
                decTrace.assign(snap.values.end() - len, snap.values.end());
                for (size_t i = 0; i < len; i++) { decTraceMarks.push_back((double)i); }
            }
            // From everything held, not only the part the trace shows, so the eye and the
            // decisions keep their scale when the span changes.
            float lo = percentileOf(snap.values, 0.01), hi = percentileOf(snap.values, 0.99);
            decRange = std::max<float>(fabsf(lo), fabsf(hi));
            for (float th : snap.thresholds) { decRange = std::max<float>(decRange, fabsf(th)); }
            if (snap.oversampled) {
                decRange = std::max<float>(decRange, std::max<float>(percentileOf(snap.samples, 0.99), -percentileOf(snap.samples, 0.01)));
            }
            decRange = std::max<float>(decRange * 1.15f, 10.0f);
            decReady = true;
        }

        // ---- Drawing.

        // The chosen one in the theme's accent colour. ButtonActive, the obvious choice,
        // is the same as a plain button in some themes, which left no way to tell which
        // view was showing.
        bool pill(const char* label, bool active) {
            if (active) {
                ImVec4 accent = ImGui::GetStyleColorVec4(ImGuiCol_CheckMark);
                ImVec4 fill = accent;
                fill.w = 0.55f;
                ImGui::PushStyleColor(ImGuiCol_Button, fill);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, fill);
            }
            bool clicked = ImGui::SmallButton(label);
            if (active) { ImGui::PopStyleColor(2); }
            return clicked;
        }

        void saveChoice(const char* key, int value) {
            core::configManager.acquire();
            core::configManager.conf[key] = value;
            core::configManager.release(true);
        }

        // Places the next item on the same line if it fits, otherwise on a new one.
        // gap is the space before it when it stays on the line; a new group of buttons
        // gets a wider one.
        void flowNext(const char* nextLabel, float gap = -1.0f) {
            float spacing = (gap < 0.0f) ? ImGui::GetStyle().ItemSpacing.x : gap;
            float w = ImGui::CalcTextSize(nextLabel, NULL, true).x + (ImGui::GetStyle().FramePadding.x * 2.0f);
            float right = ImGui::GetItemRectMax().x + spacing + w;
            float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (right < limit) { ImGui::SameLine(0.0f, spacing); }
        }

        void drawHeader() {
            ImGui::TextUnformatted("Analyzer");
            const char* views[3] = { "Inst. freq##sigan_v0", "Eye##sigan_v1", "Constellation##sigan_v2" };
            for (int i = 0; i < 3; i++) {
                flowNext(views[i]);
                if (pill(views[i], view == i)) {
                    view = i;
                    saveChoice("signalAnalyzerView", view);
                }
            }
            const char* sources[2] = { "Channel##sigan_s0", "Decoder##sigan_s1" };
            for (int i = 0; i < 2; i++) {
                flowNext(sources[i], i == 0 ? 12.0f * style::uiScale : -1.0f);
                if (pill(sources[i], source == i)) {
                    source = i;
                    saveChoice("signalAnalyzerSource", source);
                }
                if (ImGui::IsItemHovered()) {
                    style::tooltip(i == 0 ? "The signal inside the selected VFO, from the analyzer's own channel.\nSample points are the analyzer's own estimate."
                                          : "What a running DSD or oldDSD decoder actually sampled, with its own\nsample points and slicer thresholds.");
                }
            }
            float gap = 12.0f * style::uiScale;
            if (view == VIEW_FREQ) {
                // Time across the plot: less, the amount, more, and back to automatic.
                std::string text;
                char buf[40];
                if (traceSpanIdx < 0) {
                    double ms = autoTraceMs();
                    snprintf(buf, sizeof buf, ms < 10.0 ? "auto %.2f ms" : "auto %.0f ms", ms);
                }
                else {
                    double ms = TRACE_SPANS_MS[traceSpanIdx];
                    if (ms < 1.0) { snprintf(buf, sizeof buf, "%.1f ms", ms); }
                    else if (ms < 1000.0) { snprintf(buf, sizeof buf, "%.0f ms", ms); }
                    else { snprintf(buf, sizeof buf, "%.0f s", ms / 1000.0); }
                }
                text = buf;
                flowNext(" - ##sigan_tless", gap);
                if (ImGui::SmallButton(" - ##sigan_tless")) { stepTraceSpan(-1); }
                if (ImGui::IsItemHovered()) { style::tooltip("Less time across the plot"); }
                flowNext(text.c_str());
                ImGui::TextUnformatted(text.c_str());
                flowNext(" + ##sigan_tmore");
                if (ImGui::SmallButton(" + ##sigan_tmore")) { stepTraceSpan(1); }
                if (ImGui::IsItemHovered()) { style::tooltip("More time across the plot"); }
                flowNext("Auto##sigan_tauto");
                if (pill("Auto##sigan_tauto", traceSpanIdx < 0)) { traceSpanIdx = -1; saveChoice("signalAnalyzerTraceSpan", traceSpanIdx); }
                if (ImGui::IsItemHovered()) { style::tooltip("About forty symbols when there is a symbol rate, otherwise 2048 samples"); }
            }
            // No eye to widen from a decoder that passes only its decisions.
            if (view == VIEW_EYE && !(source == SOURCE_DECODER && decReady && !decWaveform())) {
                char buf[32];
                snprintf(buf, sizeof buf, "%d symbol%s", EYE_SPANS[eyeSpanIdx], EYE_SPANS[eyeSpanIdx] == 1 ? "" : "s");
                flowNext(" - ##sigan_eless", gap);
                if (ImGui::SmallButton(" - ##sigan_eless")) { eyeSpanIdx = std::max<int>(0, eyeSpanIdx - 1); saveChoice("signalAnalyzerEyeSpan", eyeSpanIdx); }
                if (ImGui::IsItemHovered()) { style::tooltip("Fewer symbols across the eye"); }
                flowNext(buf);
                ImGui::TextUnformatted(buf);
                flowNext(" + ##sigan_emore");
                if (ImGui::SmallButton(" + ##sigan_emore")) { eyeSpanIdx = std::min<int>(EYE_SPAN_COUNT - 1, eyeSpanIdx + 1); saveChoice("signalAnalyzerEyeSpan", eyeSpanIdx); }
                if (ImGui::IsItemHovered()) { style::tooltip("More symbols across the eye"); }
            }
            flowNext("Pause##sigan_pause", gap);
            if (pill(paused ? "Paused##sigan_pause" : "Pause##sigan_pause", paused)) { setPaused(!paused); }
            if (ImGui::IsItemHovered()) {
                style::tooltip(paused ? "Carry on updating" : "Hold the plot and its readouts still. Signal ID keeps measuring.");
            }
            if (view == VIEW_EYE && source == SOURCE_CHANNEL) {
                const char* of[2] = { "Freq##sigan_e0", "I##sigan_e1" };
                for (int i = 0; i < 2; i++) {
                    flowNext(of[i], i == 0 ? 12.0f * style::uiScale : -1.0f);
                    if (pill(of[i], eyeOf == i)) {
                        eyeOf = i;
                        saveChoice("signalAnalyzerEyeOf", eyeOf);
                    }
                    if (ImGui::IsItemHovered()) {
                        style::tooltip(i == 0 ? "Eye drawn on the instantaneous frequency" : "Eye drawn on the I component");
                    }
                }
            }
        }

        // Steps the Inst. freq time span. From automatic it starts at the nearest fixed
        // span to what automatic was showing, so the first press does not jump.
        void stepTraceSpan(int dir) {
            if (traceSpanIdx < 0) {
                double ms = autoTraceMs();
                if (ms <= 0.0) { ms = 20.0; }
                int nearest = 0;
                for (int i = 0; i < TRACE_SPAN_COUNT; i++) {
                    if (fabs(log(TRACE_SPANS_MS[i] / ms)) < fabs(log(TRACE_SPANS_MS[nearest] / ms))) { nearest = i; }
                }
                traceSpanIdx = nearest;
            }
            traceSpanIdx = std::clamp<int>(traceSpanIdx + dir, 0, TRACE_SPAN_COUNT - 1);
            saveChoice("signalAnalyzerTraceSpan", traceSpanIdx);
        }

        struct Plot {
            ImVec2 min, max;
            ImDrawList* dl;
            float yOf(float v, float range) const {
                float t = std::clamp<float>(v / range, -1.0f, 1.0f);
                return ((min.y + max.y) / 2.0f) - (t * (max.y - min.y) / 2.0f);
            }
        };

        void dashedH(const Plot& p, float y, ImU32 col) {
            float dash = 4.0f * style::uiScale;
            for (float x = p.min.x; x < p.max.x; x += dash * 2.0f) {
                p.dl->AddLine(ImVec2(x, y), ImVec2(std::min<float>(x + dash, p.max.x), y), col);
            }
        }

        void dashedV(const Plot& p, float x, ImU32 col) {
            float dash = 4.0f * style::uiScale;
            for (float y = p.min.y; y < p.max.y; y += dash * 2.0f) {
                p.dl->AddLine(ImVec2(x, y), ImVec2(x, std::min<float>(y + dash, p.max.y)), col);
            }
        }

        void drawTrace(const Plot& p, const std::vector<float>& y, const std::vector<double>& marks, float range,
                       const std::vector<float>& levels, const float* thresholds, bool stepped) {
            ImU32 grid = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            ImU32 levelCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImU32 threshCol = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftMinHoldColor);
            ImU32 lineCol = ImGui::GetColorU32(ImGuiCol_PlotLines);
            float midY = (p.min.y + p.max.y) / 2.0f;
            p.dl->AddLine(ImVec2(p.min.x, midY), ImVec2(p.max.x, midY), grid);
            for (float l : levels) { dashedH(p, p.yOf(l, range), levelCol); }
            if (thresholds) {
                for (int i = 0; i < 3; i++) { dashedH(p, p.yOf(thresholds[i], range), threshCol); }
            }
            if (y.size() < 2) { return; }
            float w = p.max.x - p.min.x;
            float xScale = w / (float)(y.size() - 1);
            float thick = 1.2f * style::uiScale;
            if (stepped) {
                for (size_t i = 0; i < y.size(); i++) {
                    float x0 = p.min.x + (xScale * (float)i);
                    float x1 = std::min<float>(x0 + xScale, p.max.x);
                    float yy = p.yOf(y[i], range);
                    p.dl->AddLine(ImVec2(x0, yy), ImVec2(x1, yy), lineCol, thick);
                }
            }
            else {
                // One vertex per pixel at most, keeping the extremes, so spikes survive.
                size_t cols = (size_t)std::max<float>(2.0f, w);
                if (y.size() <= cols) {
                    for (size_t i = 0; i + 1 < y.size(); i++) {
                        p.dl->AddLine(ImVec2(p.min.x + (xScale * i), p.yOf(y[i], range)),
                                      ImVec2(p.min.x + (xScale * (i + 1)), p.yOf(y[i + 1], range)), lineCol, thick);
                    }
                }
                else {
                    float prevY = p.yOf(y[0], range);
                    for (size_t c = 0; c < cols; c++) {
                        size_t a = c * y.size() / cols;
                        size_t b = std::max<size_t>(a + 1, (c + 1) * y.size() / cols);
                        float lo = y[a], hi = y[a];
                        for (size_t i = a; i < b && i < y.size(); i++) { lo = std::min<float>(lo, y[i]); hi = std::max<float>(hi, y[i]); }
                        float x = p.min.x + (float)c;
                        p.dl->AddLine(ImVec2(x, p.yOf(hi, range)), ImVec2(x, p.yOf(lo, range)), lineCol, thick);
                        p.dl->AddLine(ImVec2(x - 1.0f, prevY), ImVec2(x, p.yOf(y[a], range)), lineCol, thick);
                        prevY = p.yOf(y[std::min<size_t>(b, y.size()) - 1], range);
                    }
                }
            }
            ImU32 markCol = ImGui::GetColorU32(ImGuiCol_Text);
            float dot = 1.5f * style::uiScale;
            // Dots closer than a few pixels apart only smear the trace.
            if ((float)marks.size() > w / 4.0f) { return; }
            for (double m : marks) {
                float x = p.min.x + (xScale * (float)m);
                if (x < p.min.x || x > p.max.x) { continue; }
                p.dl->AddCircleFilled(ImVec2(x, p.yOf(interpAt(y, m), range)), dot, markCol, 6);
            }
        }

        void drawEye(const Plot& p, const std::vector<float>& y, const std::vector<double>& instants, double T, float range,
                     const std::vector<float>& levels, const float* thresholds) {
            ImU32 levelCol = ImGui::GetColorU32(ImGuiCol_TextDisabled);
            ImU32 threshCol = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftMinHoldColor);
            ImVec4 lc = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
            lc.w = 0.25f;
            ImU32 traceCol = ImGui::ColorConvertFloat4ToU32(lc);
            float midX = (p.min.x + p.max.x) / 2.0f;
            for (float l : levels) { dashedH(p, p.yOf(l, range), levelCol); }
            if (thresholds) {
                for (int i = 0; i < 3; i++) { dashedH(p, p.yOf(thresholds[i], range), threshCol); }
            }
            if (T < 2.0 || instants.size() < 4) { return; }
            const int symbols = EYE_SPANS[eyeSpanIdx];
            const int points = (EYE_POINTS_PER_SYMBOL * symbols) + 1;
            const double half = T * symbols / 2.0;
            size_t first = instants.size() > (size_t)MAX_EYE_TRACES ? instants.size() - MAX_EYE_TRACES : 0;
            float w = p.max.x - p.min.x;
            // Faint lines at the symbol boundaries either side of the sample line.
            ImU32 gridCol = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            // The sample line is in the middle, so the boundaries sit half a symbol either
            // side of it and every symbol after that.
            for (int j = -symbols; j <= symbols; j++) {
                double off = ((double)j + 0.5) * T;
                if (fabs(off) >= half) { continue; }
                float x = p.min.x + (float)((off + half) / (2.0 * half)) * w;
                p.dl->AddLine(ImVec2(x, p.min.y), ImVec2(x, p.max.y), gridCol);
            }
            for (size_t k = first; k < instants.size(); k++) {
                double centre = instants[k];
                if (centre - half < 0.0 || centre + half > (double)(y.size() - 1)) { continue; }
                float prevX = 0.0f, prevY = 0.0f;
                for (int s = 0; s < points; s++) {
                    double t = centre - half + (2.0 * half * s / (points - 1));
                    float x = p.min.x + (w * (float)s / (float)(points - 1));
                    float yy = p.yOf(interpAt(y, t), range);
                    if (s > 0) { p.dl->AddLine(ImVec2(prevX, prevY), ImVec2(x, yy), traceCol, style::uiScale); }
                    prevX = x;
                    prevY = yy;
                }
            }
            dashedV(p, midX, ImGui::GetColorU32(ImGuiCol_Text));
        }

        void drawConstellation(const Plot& area, const std::vector<dsp::complex_t>& pts) {
            float side = std::min<float>(area.max.x - area.min.x, area.max.y - area.min.y);
            ImVec2 c((area.min.x + area.max.x) / 2.0f, (area.min.y + area.max.y) / 2.0f);
            ImVec2 mn(c.x - side / 2.0f, c.y - side / 2.0f), mx(c.x + side / 2.0f, c.y + side / 2.0f);
            ImDrawList* dl = area.dl;
            ImU32 grid = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftGridColor);
            dl->AddLine(ImVec2(mn.x, c.y), ImVec2(mx.x, c.y), grid);
            dl->AddLine(ImVec2(c.x, mn.y), ImVec2(c.x, mx.y), grid);
            // The outer ring is twice the average magnitude.
            float r = side / 2.0f;
            dl->AddCircle(c, r * 0.5f, grid, 64);
            dl->AddCircle(c, r, grid, 64);
            ImVec4 dc = ImGui::GetStyleColorVec4(ImGuiCol_PlotLines);
            dc.w = 0.45f;
            ImU32 dotCol = ImGui::ColorConvertFloat4ToU32(dc);
            float dot = std::max<float>(1.5f, 1.5f * style::uiScale);
            for (const auto& pt : pts) {
                float x = c.x + (pt.re * r * 0.5f);
                float y = c.y - (pt.im * r * 0.5f);
                dl->AddRectFilled(ImVec2(x - dot / 2.0f, y - dot / 2.0f), ImVec2(x + dot / 2.0f, y + dot / 2.0f), dotCol);
            }
        }

        // Decisions from a decoder as a histogram: how many landed at each value, with
        // the decoder's thresholds across it. Tall narrow peaks well clear of the orange
        // lines are decisions with room to spare; anything piling up against a line is a
        // decision that could have gone either way.
        void drawDecisions(const Plot& p, const std::vector<float>& values, float range, const float* thresholds) {
            ImU32 threshCol = ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftMinHoldColor);
            ImU32 barCol = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
            float midX = (p.min.x + p.max.x) / 2.0f;
            float half = (p.max.x - p.min.x) / 2.0f;
            auto xOf = [&](float v) { return midX + (std::clamp<float>(v / range, -1.0f, 1.0f) * half); };
            const int BINS = 96;
            std::vector<int> counts(BINS, 0);
            size_t first = values.size() > 2048 ? values.size() - 2048 : 0;
            for (size_t i = first; i < values.size(); i++) {
                int b = (int)(((values[i] / range) + 1.0f) * 0.5f * BINS);
                if (b >= 0 && b < BINS) { counts[b]++; }
            }
            int maxC = std::max<int>(1, *std::max_element(counts.begin(), counts.end()));
            float binW = (p.max.x - p.min.x) / BINS;
            float h = (p.max.y - p.min.y) - (12.0f * style::uiScale);
            for (int b = 0; b < BINS; b++) {
                if (counts[b] == 0) { continue; }
                float x0 = p.min.x + (binW * b);
                float top = p.max.y - (h * (float)counts[b] / (float)maxC);
                p.dl->AddRectFilled(ImVec2(x0 + 1.0f, top), ImVec2(x0 + binW - 1.0f, p.max.y), barCol);
            }
            for (int i = 0; i < 3; i++) {
                float x = xOf(thresholds[i]);
                float dash = 4.0f * style::uiScale;
                for (float y = p.min.y; y < p.max.y; y += dash * 2.0f) {
                    p.dl->AddLine(ImVec2(x, y), ImVec2(x, std::min<float>(y + dash, p.max.y)), threshCol);
                }
            }
        }

        // Lays the readouts out in lines across width, a whole readout at a time, and
        // draws them when draw is set. Returns the lines they take, up to maxLines;
        // anything past that is left off.
        int readouts(float width, const std::vector<std::string>& parts, int maxLines, bool draw) {
            if (parts.empty()) { return 0; }
            ImGui::PushFont(style::tinyFont);
            float spacing = 10.0f * style::uiScale;
            float used = 0.0f;
            int lines = 1;
            for (size_t i = 0; i < parts.size(); i++) {
                float w = ImGui::CalcTextSize(parts[i].c_str()).x;
                bool sameLine = true;
                if (i > 0) {
                    if (used + spacing + w > width) {
                        if (lines >= maxLines) { break; }
                        lines++;
                        used = 0.0f;
                        sameLine = false;
                    }
                    else { used += spacing; }
                }
                if (draw) {
                    if (i > 0 && sameLine) { ImGui::SameLine(0.0f, spacing); }
                    ImGui::TextDisabled("%s", parts[i].c_str());
                }
                used += w;
            }
            ImGui::PopFont();
            return lines;
        }

        static std::string hz(double v, bool sign = true) {
            char buf[48];
            if (fabs(v) >= 1000.0) { snprintf(buf, sizeof buf, sign ? "%+.2f kHz" : "%.2f kHz", v / 1000.0); }
            else { snprintf(buf, sizeof buf, sign ? "%+.0f Hz" : "%.0f Hz", v); }
            return std::string(buf);
        }

        std::vector<std::string> channelReadouts() {
            std::vector<std::string> out;
            char buf[96];
            if (cv.meas.haveSymbolRate) {
                snprintf(buf, sizeof buf, "%.0f Bd (line %.0f dB)", cv.meas.symbolRate, cv.meas.lineDb);
                out.push_back(buf);
            }
            else {
                out.push_back("no symbol-rate line");
            }
            if (view == VIEW_CONST) {
                if (cv.constGroups > 0) {
                    snprintf(buf, sizeof buf, "%d phase groups", cv.constGroups);
                    out.push_back(buf);
                }
                if (cv.meas.haveSymbolRate) {
                    snprintf(buf, sizeof buf, "magnitude spread %.0f%%", cv.constSpread * 100.0f);
                    out.push_back(buf);
                }
                if (cv.meas.haveSymbolRate) {
                    if (cv.rotFound) { out.push_back("turning " + hz(cv.rotHz) + ", held still"); }
                    else { out.push_back("no steady turning to hold still"); }
                }
                return out;
            }
            if (cv.meas.levels > 0) {
                snprintf(buf, sizeof buf, "%d level%s", cv.meas.levels, cv.meas.levels == 1 ? "" : "s");
                out.push_back(buf);
                if (cv.meas.levels >= 2) { out.push_back("deviation " + std::string("\xC2\xB1") + hz(cv.meas.deviationHz, false)); }
            }
            if (cv.meas.haveEye) {
                snprintf(buf, sizeof buf, "eye %.0f%% open", cv.meas.eyeOpening * 100.0f);
                out.push_back(buf);
            }
            if (cv.meas.haveEdges) {
                snprintf(buf, sizeof buf, "edges %.0f%% of a symbol", cv.meas.edgeFraction * 100.0f);
                out.push_back(buf);
            }
            if (cv.meas.haveOffset) { out.push_back("average " + hz(cv.meas.offsetHz)); }
            return out;
        }

        std::vector<std::string> decoderReadouts() {
            std::vector<std::string> out;
            char buf[96];
            out.push_back(snap.source);
            if (snap.symbolRate > 0.0) {
                snprintf(buf, sizeof buf, "%.0f Bd", snap.symbolRate);
                out.push_back(buf);
            }
            if (!decLevels.empty()) {
                snprintf(buf, sizeof buf, "%d level%s", (int)decLevels.size(), decLevels.size() == 1 ? "" : "s");
                out.push_back(buf);
            }
            // A decoder that passes only its decisions has no sample points of its own to
            // speak of: the opening is measured on the decisions themselves.
            if (decHaveEye) {
                snprintf(buf, sizeof buf, decWaveform() ? "eye %.0f%% open at its samples" : "decisions %.0f%% open", decOpening * 100.0f);
                out.push_back(buf);
            }
            if (decHaveWidest) {
                if (fabsf(decWidest) < 0.049f) { out.push_back("widest at its samples"); }
                else {
                    snprintf(buf, sizeof buf, "widest %.2f symbol %s", fabsf(decWidest), decWidest > 0.0f ? "later" : "earlier");
                    out.push_back(buf);
                }
            }
            if (decNearThreshold >= 0.0f) {
                snprintf(buf, sizeof buf, "%.1f%% near a threshold", decNearThreshold * 100.0f);
                out.push_back(buf);
            }
            if (decHaveEdges) {
                snprintf(buf, sizeof buf, "edges %.0f%% of a symbol", decEdges * 100.0f);
                out.push_back(buf);
            }
            return out;
        }

        bool symbolsWanted = false;
        std::string selName;
        double selOffset = 0.0;
        double selBandwidth = 0.0;

        dsp::channel::RxVFO* channel = nullptr;
        ChannelSink sink;
        bool sinkInit = false;
        double chanRate = 48000.0;
        double chanBandwidth = 0.0;
        double chanOffset = 0.0;
        double lastAnalysis = 0.0;

        fftwf_complex* fftIn = nullptr;
        fftwf_complex* fftOut = nullptr;
        fftwf_plan fftPlan = nullptr;
        std::vector<float> powerScratch;
        std::vector<double> prefix;
        fftwf_complex* rotIn = nullptr;
        fftwf_complex* rotOut = nullptr;
        fftwf_plan rotPlan = nullptr;

        std::vector<dsp::complex_t> iq, iqLong;
        std::vector<float> featF, featA;
        ChannelView cv;          // what is drawn
        ChannelView cvScratch;   // filled instead while paused
        std::vector<dsp::complex_t> frozenIq;   // everything the channel held when paused
        double frozenRate = 48000.0;
        int frozenSpanIdx = -1;  // the span cv was last drawn at from frozenIq
        int decSpanIdx = -1;     // the span decTrace was last made at

        symboltap::Snapshot snap;
        bool decReady = false;
        std::vector<float> decLevels;
        bool decHaveEye = false;
        float decOpening = 0.0f;
        bool decHaveWidest = false;
        float decWidest = 0.0f;
        bool decHaveEdges = false;
        float decEdges = 0.0f;
        float decNearThreshold = -1.0f;
        double decT = 0.0;
        std::vector<float> decTrace;
        std::vector<double> decTraceMarks;
        float decRange = 1000.0f;
    };

    void Analyzer::draw() {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        if (avail.x < 80.0f || avail.y < 60.0f) { return; }
        ImVec2 origin = ImGui::GetCursorScreenPos();

        drawHeader();

        ImGui::PushFont(style::tinyFont);
        float tinyLine = ImGui::GetTextLineHeightWithSpacing();
        ImGui::PopFont();

        // The readouts first, so the plot leaves exactly the room they need. At most a
        // third of the panel, so a short panel still has a plot.
        bool running = gui::mainWindow.sdrIsRunning();
        std::vector<std::string> parts;
        if (running && source == SOURCE_CHANNEL && !gui::waterfall.selectedVFO.empty() && cv.ready) { parts = channelReadouts(); }
        else if (running && source == SOURCE_DECODER && decReady) { parts = decoderReadouts(); }
        int maxLines = std::max<int>(1, (int)((avail.y / 3.0f) / tinyLine));
        int lines = readouts(avail.x, parts, maxLines, false);

        ImVec2 top = ImGui::GetCursorScreenPos();
        float plotH = (origin.y + avail.y) - top.y - ((float)std::max<int>(lines, 1) * tinyLine) - (4.0f * style::uiScale);
        if (plotH < 30.0f) { return; }
        Plot p;
        p.min = ImVec2(origin.x, top.y);
        p.max = ImVec2(origin.x + avail.x, top.y + plotH);
        p.dl = ImGui::GetWindowDrawList();
        p.dl->AddRect(p.min, p.max, ImGui::ColorConvertFloat4ToU32(gui::themeManager.fftBorderColor));

        // Wrapped to the plot, which can be narrow with other panels beside it.
        auto message = [&](const char* text) {
            float wrap = std::max<float>(40.0f, (p.max.x - p.min.x) - (12.0f * style::uiScale));
            ImVec2 ts = ImGui::CalcTextSize(text, NULL, false, wrap);
            // From the top when it is taller than the plot, so the start of it shows.
            float y = std::max<float>(p.min.y + (2.0f * style::uiScale), (p.min.y + p.max.y - ts.y) / 2.0f);
            p.dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                          ImVec2((p.min.x + p.max.x - ts.x) / 2.0f, y),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), text, NULL, wrap);
        };

        bool plotEmpty = false;
        p.dl->PushClipRect(p.min, p.max, true);
        if (!running) {
            message("radio stopped");
        }
        else if (source == SOURCE_CHANNEL) {
            if (gui::waterfall.selectedVFO.empty()) { message("no VFO selected"); }
            else if (!cv.ready) { message("collecting"); }
            else {
                if (view == VIEW_FREQ) {
                    drawTrace(p, cv.trace, cv.traceMarks, cv.traceRange, cv.levels, nullptr, false);
                }
                else if (view == VIEW_EYE) {
                    const std::vector<double>& instants = (eyeOf == EYE_OF_FREQ) ? cv.instants : cv.constInstants;
                    if (cv.T < 2.0) {
                        message("No symbol rate found. An eye needs a signal sent as symbols at a steady rate.");
                        plotEmpty = true;
                    }
                    else if (instants.size() < 4) {
                        // A slow rate in a wide channel: a symbol-rate line, but too few
                        // whole symbols in the window to lay over each other.
                        message("Too few symbols in the window for an eye.");
                        plotEmpty = true;
                    }
                    else if (eyeOf == EYE_OF_FREQ) {
                        drawEye(p, cv.freq, cv.instants, cv.T, cv.freqRange, cv.levels, nullptr);
                    }
                    else {
                        float iRange = std::max<float>(percentileOf(cv.iPart, 0.99), -percentileOf(cv.iPart, 0.01)) * 1.15f;
                        drawEye(p, cv.iPart, cv.constInstants, cv.T, std::max<float>(iRange, 0.1f), std::vector<float>(), nullptr);
                    }
                }
                else {
                    drawConstellation(p, cv.constPoints);
                }
            }
        }
        else {
            if (!decReady) { message("no decoder publishing - choose DSD or oldDSD in Radio"); }
            else {
                if (view == VIEW_FREQ) {
                    drawTrace(p, decTrace, decTraceMarks, decRange, decLevels, snap.thresholds, !decWaveform());
                }
                else if (view == VIEW_EYE) {
                    if (!decWaveform()) {
                        message((snap.source + " passes only its decisions, not a waveform to draw an eye from.").c_str());
                        plotEmpty = true;
                    }
                    else { drawEye(p, snap.samples, snap.marks, decT, decRange, decLevels, snap.thresholds); }
                }
                else {
                    drawDecisions(p, snap.values, decRange, snap.thresholds);
                }
            }
        }
        p.dl->PopClipRect();

        // The scale, top left inside the plot.
        if (!plotEmpty && running && ((source == SOURCE_CHANNEL && cv.ready) || (source == SOURCE_DECODER && decReady))) {
            char scale[96];
            char eyeSpan[32];
            snprintf(eyeSpan, sizeof eyeSpan, "%d symbol%s across", EYE_SPANS[eyeSpanIdx], EYE_SPANS[eyeSpanIdx] == 1 ? "" : "s");
            if (view == VIEW_CONST && source == SOURCE_CHANNEL) {
                snprintf(scale, sizeof scale, "outer ring 2x average");
            }
            else if (view == VIEW_EYE && source == SOURCE_CHANNEL && eyeOf == EYE_OF_I) {
                snprintf(scale, sizeof scale, "I, %s", eyeSpan);
            }
            else {
                float range = (source == SOURCE_DECODER) ? decRange : (view == VIEW_EYE) ? cv.freqRange : cv.traceRange;
                std::string r = hz(range, false);
                if (view == VIEW_CONST) { snprintf(scale, sizeof scale, "decisions, edges \xC2\xB1%s", r.c_str()); }
                else if (view == VIEW_EYE) { snprintf(scale, sizeof scale, "\xC2\xB1%s, %s", r.c_str(), eyeSpan); }
                else { snprintf(scale, sizeof scale, "\xC2\xB1%s", r.c_str()); }
            }
            if (paused) { strncat(scale, "  paused", sizeof(scale) - strlen(scale) - 1); }
            ImGui::PushFont(style::tinyFont);
            p.dl->AddText(ImVec2(p.min.x + (3.0f * style::uiScale), p.min.y + (1.0f * style::uiScale)),
                          ImGui::GetColorU32(ImGuiCol_TextDisabled), scale);
            ImGui::PopFont();
        }

        ImGui::SetCursorScreenPos(ImVec2(origin.x, p.max.y + (2.0f * style::uiScale)));
        readouts(avail.x, parts, maxLines, true);
    }

    Analyzer* analyzer = nullptr;
    EventHandler<ImGuiContext*> waterfallDrawnHandler;
}

namespace sigan {
    void init() {
        if (analyzer) { return; }
        analyzer = new Analyzer();
        core::configManager.acquire();
        auto& conf = core::configManager.conf;
        if (conf.contains("showSignalAnalyzer")) { analyzer->shown = conf["showSignalAnalyzer"]; }
        if (conf.contains("signalAnalyzerView")) { analyzer->view = std::clamp<int>(conf["signalAnalyzerView"], 0, 2); }
        if (conf.contains("signalAnalyzerSource")) { analyzer->source = std::clamp<int>(conf["signalAnalyzerSource"], 0, 1); }
        if (conf.contains("signalAnalyzerEyeOf")) { analyzer->eyeOf = std::clamp<int>(conf["signalAnalyzerEyeOf"], 0, 1); }
        if (conf.contains("signalAnalyzerTraceSpan")) { analyzer->traceSpanIdx = std::clamp<int>(conf["signalAnalyzerTraceSpan"], -1, TRACE_SPAN_COUNT - 1); }
        if (conf.contains("signalAnalyzerEyeSpan")) { analyzer->eyeSpanIdx = std::clamp<int>(conf["signalAnalyzerEyeSpan"], 0, EYE_SPAN_COUNT - 1); }
        core::configManager.release();

        waterfallDrawnHandler.ctx = analyzer;
        waterfallDrawnHandler.handler = [](ImGuiContext* gctx, void* ctx) {
            ((Analyzer*)ctx)->tick();
        };
        gui::mainWindow.onWaterfallDrawn.bindHandler(&waterfallDrawnHandler);
    }

    bool isShown() { return analyzer && analyzer->shown; }

    void setShown(bool shown) {
        if (!analyzer) { return; }
        analyzer->shown = shown;
        core::configManager.acquire();
        core::configManager.conf["showSignalAnalyzer"] = shown;
        core::configManager.release(true);
    }

    void request() {
        if (analyzer) { analyzer->lastRequest = ImGui::GetTime(); }
    }

    bool getMeasurements(Measurements& out) {
        if (!analyzer || !analyzer->meas.running) { return false; }
        out = analyzer->meas;
        return true;
    }
}
