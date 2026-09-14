#pragma once
#include <string>
#include <vector>

// What a decoder actually sampled, handed to the signal analyzer.
//
// The analyzer can look at the signal in the VFO on its own and work out a symbol rate
// and a sampling point, but that is its own guess. When a decoder fails on a clean
// signal the cause is usually in the decoder's timing or its slicer thresholds, and
// only the decoder knows those. A decoder that publishes here lets the analyzer draw
// the eye around the decoder's own sample instants, with the decoder's own thresholds
// across it.
//
// Publishing is from the decoder's DSP thread. Everything is converted to hertz of
// deviation on the way in, so the analyzer never needs to know a decoder's units.
namespace symboltap {
    // True while the analyzer is showing decoder data. Decoders check this before
    // copying anything, so publishing costs one call while nobody is looking.
    bool wanted();
    void setWanted(bool wanted);

    // One symbol, with the samples the decoder read for it, oldest first.
    // samplePoint is where in those samples the decoder took its decision, and may fall
    // between two of them. thresholds are the slicer's lower, middle and upper
    // decision levels. All values are in the decoder's units; hzPerUnit converts them.
    void publishSymbol(const char* source, const float* samples, int count, float samplePoint, float value,
                       const float thresholds[3], float hzPerUnit, double sampleRate);

    // A run of symbol values from a decoder that has no oversampled waveform to offer,
    // one value per symbol, after its clock recovery.
    void publishSymbols(const char* source, const float* values, int count,
                        const float thresholds[3], float hzPerUnit, double symbolRate);

    struct Snapshot {
        std::string source;
        double age = 1e9;              // seconds since the last publish
        double sampleRate = 0.0;       // of samples, when oversampled
        double symbolRate = 0.0;
        bool oversampled = false;      // samples carries a waveform, not one value per symbol
        std::vector<float> samples;    // Hz
        std::vector<double> marks;     // sample-point positions, as indices into samples
        std::vector<float> values;     // Hz, one per mark
        float thresholds[3] = { 0.0f, 0.0f, 0.0f }; // Hz
    };

    // The latest data, oldest first. False when nothing has been published.
    bool snapshot(Snapshot& out);
}
