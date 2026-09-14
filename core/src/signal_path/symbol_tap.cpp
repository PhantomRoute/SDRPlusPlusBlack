#include <signal_path/symbol_tap.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>

namespace symboltap {
    namespace {
        // About a third of a second of waveform at 48 kHz, and the symbols that go with it.
        const size_t MAX_SAMPLES = 16384;
        const size_t MAX_SYMBOLS = 4096;

        std::atomic<bool> wantedFlag{ false };
        std::mutex mtx;

        std::string source;
        double sampleRate = 0.0;
        double symbolRate = 0.0;
        bool oversampled = false;
        float thresholds[3] = { 0.0f, 0.0f, 0.0f };
        std::chrono::steady_clock::time_point lastPublish;
        bool havePublished = false;

        // Absolute sample index of samples.front(), so a mark stays attached to the
        // sample it was taken at as old samples fall off the front.
        long long firstSample = 0;
        std::deque<float> samples;
        std::deque<double> marks;   // absolute sample positions
        std::deque<float> values;

        // A different decoder, or the same one switching between an oversampled
        // waveform and bare symbols, starts the history over: the two cannot be mixed.
        void restartIfChanged(const char* src, bool isOversampled) {
            if (havePublished && source == src && oversampled == isOversampled) { return; }
            source = src;
            oversampled = isOversampled;
            samples.clear();
            marks.clear();
            values.clear();
            firstSample = 0;
            havePublished = true;
        }

        void trim() {
            while (samples.size() > MAX_SAMPLES) {
                samples.pop_front();
                firstSample++;
            }
            while (!marks.empty() && (marks.size() > MAX_SYMBOLS || marks.front() < (double)firstSample)) {
                marks.pop_front();
                values.pop_front();
            }
        }
    }

    bool wanted() { return wantedFlag.load(std::memory_order_relaxed); }

    void setWanted(bool w) {
        wantedFlag.store(w, std::memory_order_relaxed);
        if (!w) {
            std::lock_guard<std::mutex> lck(mtx);
            samples.clear();
            marks.clear();
            values.clear();
            firstSample = 0;
            havePublished = false;
        }
    }

    void publishSymbol(const char* src, const float* in, int count, float samplePoint, float value,
                       const float th[3], float hzPerUnit, double rate) {
        if (!wanted() || count <= 0 || in == nullptr) { return; }
        std::lock_guard<std::mutex> lck(mtx);
        restartIfChanged(src, true);
        long long base = firstSample + (long long)samples.size();
        for (int i = 0; i < count; i++) { samples.push_back(in[i] * hzPerUnit); }
        marks.push_back((double)base + (double)samplePoint);
        values.push_back(value * hzPerUnit);
        for (int i = 0; i < 3; i++) { thresholds[i] = th[i] * hzPerUnit; }
        sampleRate = rate;
        lastPublish = std::chrono::steady_clock::now();
        trim();
    }

    void publishSymbols(const char* src, const float* in, int count, const float th[3], float hzPerUnit, double rate) {
        if (!wanted() || count <= 0 || in == nullptr) { return; }
        std::lock_guard<std::mutex> lck(mtx);
        restartIfChanged(src, false);
        for (int i = 0; i < count; i++) {
            long long index = firstSample + (long long)samples.size();
            samples.push_back(in[i] * hzPerUnit);
            marks.push_back((double)index);
            values.push_back(in[i] * hzPerUnit);
        }
        for (int i = 0; i < 3; i++) { thresholds[i] = th[i] * hzPerUnit; }
        symbolRate = rate;
        sampleRate = rate;
        lastPublish = std::chrono::steady_clock::now();
        trim();
    }

    bool snapshot(Snapshot& out) {
        std::lock_guard<std::mutex> lck(mtx);
        if (!havePublished || samples.empty()) { return false; }
        out.source = source;
        out.age = std::chrono::duration<double>(std::chrono::steady_clock::now() - lastPublish).count();
        out.oversampled = oversampled;
        out.sampleRate = sampleRate;
        out.samples.assign(samples.begin(), samples.end());
        out.marks.resize(marks.size());
        for (size_t i = 0; i < marks.size(); i++) { out.marks[i] = marks[i] - (double)firstSample; }
        out.values.assign(values.begin(), values.end());
        for (int i = 0; i < 3; i++) { out.thresholds[i] = thresholds[i]; }

        // The symbol rate of an oversampled source, from how far apart its decisions are.
        // A median, since the old decoder adds or drops a sample now and then to keep time.
        if (oversampled && out.marks.size() >= 8 && sampleRate > 0.0) {
            std::vector<double> gaps;
            gaps.reserve(out.marks.size());
            for (size_t i = 1; i < out.marks.size(); i++) { gaps.push_back(out.marks[i] - out.marks[i - 1]); }
            std::sort(gaps.begin(), gaps.end());
            double gap = gaps[gaps.size() / 2];
            out.symbolRate = (gap > 0.0) ? sampleRate / gap : 0.0;
        }
        else {
            out.symbolRate = symbolRate;
        }
        return true;
    }
}
